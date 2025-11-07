#!/usr/bin/env python3
import argparse
import hashlib
import os
import random
import threading
import queue
from itertools import product

import boto3
from botocore.client import Config

# ---------- Helpers ----------
def hmod(key: str, m: int) -> int:
    """Stable, uniform group assignment."""
    return int(hashlib.blake2b(key.encode("utf-8"), digest_size=8).hexdigest(), 16) % m


def build_s3_client(endpoint, access_key, secret_key, max_pool, read_timeout, connect_timeout):
    cfg = Config(
        signature_version="s3v4",
        connect_timeout=connect_timeout,
        read_timeout=read_timeout,
        retries={"mode": "standard", "max_attempts": 10},
        max_pool_connections=max_pool,
    )
    return boto3.client(
        "s3",
        config=cfg,
        aws_access_key_id=access_key,
        aws_secret_access_key=secret_key,
        endpoint_url=endpoint,
        use_ssl=str(endpoint).startswith("https://"),
        verify=True,
        region_name="us-sea-3",
    )


# ---------- Main ----------
def main():
    p = argparse.ArgumentParser(
        description="Stream-tag objects with ArchiveRetentionDays=1..N using a concurrent producer-consumer pipeline."
    )
    p.add_argument("--bucket", required=True)
    p.add_argument("--endpoint", default="https://us-sea-3.linodeobjects.com")
    p.add_argument("--access-key", required=True)
    p.add_argument("--secret-key", required=True)

    p.add_argument("--num-tags", type=int, default=60, help="Number of distinct tag values (1..num-tags).")
    p.add_argument("--total", type=int, default=1_000_000, help="Max objects to process.")
    p.add_argument("--prefix", default="", help="Optional common prefix to limit listing scope.")
    p.add_argument("--page-size", type=int, default=1000, help="S3 list_objects_v2 page size (<=1000).")

    p.add_argument("--max-workers", type=int, default=64, help="Concurrent taggers (consumer threads).")
    p.add_argument("--max-pool", type=int, default=128, help="botocore max_pool_connections (>= max-workers*2 ideal).")
    p.add_argument("--queue-size", type=int, default=100000, help="Bounded queue size for producer->consumer pipeline.")
    p.add_argument("--prefix-width", type=int, default=0,
                   help="If >0, fan-out listing across hex shards: 1=>16, 2=>256 (00..ff).")

    p.add_argument("--verify-rate", type=float, default=0.0,
                   help="Fraction [0..1] of objects to verify with GET tagging (0 disables).")
    p.add_argument("--dry-run", action="store_true", help="Plan and count only; do not PUT/GET tags.")
    p.add_argument("--randomize", action="store_true",
                   help="Randomly assign tags instead of hash(key) for distribution.")
    p.add_argument("--read-timeout", type=int, default=60)
    p.add_argument("--connect-timeout", type=int, default=5)
    p.add_argument("--seed", default="0", help="Random seed (for --randomize and verify sampling).")

    args = p.parse_args()

    # --- Normalize / guards
    tags_n = max(1, args.num_tags)
    target_total = max(1, args.total)
    page_size = min(max(1, args.page_size), 1000)
    max_workers = max(1, args.max_workers)
    max_pool = max(args.max_pool, max_workers * 2)

    random.seed(args.seed)

    s3 = build_s3_client(
        endpoint=args.endpoint,
        access_key=args.access_key,
        secret_key=args.secret_key,
        max_pool=max_pool,
        read_timeout=args.read_timeout,
        connect_timeout=args.connect_timeout,
    )

    # Counters / state
    lock = threading.Lock()
    submitted = 0
    done_ok = 0
    done_fail = 0
    group_ok = [0] * tags_n

    work_q = queue.Queue(maxsize=max(1000, args.queue_size))
    stop_flag = threading.Event()   # stop listing when total reached
    done_event = threading.Event()  # producers finished and queue drained

    def assign_group(key: str) -> int:
        if args.randomize:
            return random.randint(0, tags_n - 1)
        return hmod(key, tags_n)

    def add_and_maybe_verify(keyname: str, group_idx: int):
        nonlocal done_ok, done_fail
        if args.dry_run:
            with lock:
                done_ok += 1
                group_ok[group_idx] += 1
            return

        try:
            tag_kv = {"Key": "ArchiveRetentionDays", "Value": str(group_idx + 1)}
            put_resp = s3.put_object_tagging(
                Bucket=args.bucket, Key=keyname, Tagging={"TagSet": [tag_kv]}
            )
            if put_resp["ResponseMetadata"]["HTTPStatusCode"] != 200:
                raise RuntimeError(f"PUT tag HTTP != 200 for {keyname}")

            if args.verify_rate > 0 and random.random() < args.verify_rate:
                get_resp = s3.get_object_tagging(Bucket=args.bucket, Key=keyname)
                if get_resp["ResponseMetadata"]["HTTPStatusCode"] != 200:
                    raise RuntimeError(f"GET tag HTTP != 200 for {keyname}")
                ok = any(
                    t["Key"] == "ArchiveRetentionDays"
                    and t["Value"] == str(group_idx + 1)
                    for t in get_resp.get("TagSet", [])
                )
                if not ok:
                    raise AssertionError(f"Tag mismatch for {keyname}")

            with lock:
                done_ok += 1
                group_ok[group_idx] += 1
        except Exception:
            with lock:
                done_fail += 1

    def list_prefix(shard: str):
        nonlocal submitted
        paginator = s3.get_paginator("list_objects_v2")
        # Combine base prefix with shard prefix (lowercase hex shard recommended)
        eff_prefix = (args.prefix or "") + shard
        for page in paginator.paginate(
            Bucket=args.bucket,
            Prefix=eff_prefix,
            PaginationConfig={"PageSize": page_size},
        ):
            if stop_flag.is_set() or done_event.is_set():
                break
            for obj in page.get("Contents", []):
                if stop_flag.is_set() or done_event.is_set():
                    break
                key = obj["Key"]
                g = assign_group(key)
                with lock:
                    if submitted >= target_total:
                        stop_flag.set()
                        break
                    submitted += 1
                # Backpressure if consumers lag
                work_q.put((key, g))
                if submitted >= target_total:
                    stop_flag.set()
                    break
            if stop_flag.is_set() or done_event.is_set():
                break

    def consumer():
        # Drain all queued work; exit after producers are done AND queue is empty
        while not (done_event.is_set() and work_q.empty()):
            try:
                key, g = work_q.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                add_and_maybe_verify(key, g)
            finally:
                work_q.task_done()

    # Build shard set (optional)
    if args.prefix_width > 0:
        hexchars = "0123456789abcdef"
        shards = [''.join(t) for t in product(hexchars, repeat=args.prefix_width)]
    else:
        shards = [""]

    print(
        f"Bucket='{args.bucket}' Endpoint='{args.endpoint}' "
        f"Target={target_total} NumTags={tags_n} Prefix='{args.prefix}' "
        f"Shards={len(shards)} PageSize={page_size} DryRun={args.dry_run} "
        f"VerifyRate={args.verify_rate} MaxWorkers={max_workers} MaxPool={max_pool}"
    )

    # Start consumers
    consumers = []
    for _ in range(max_workers):
        t = threading.Thread(target=consumer, daemon=True)
        t.start()
        consumers.append(t)

    # Start producers (can start all; S3 will rate-limit fairly)
    producers = []
    for shard in shards:
        t = threading.Thread(target=list_prefix, args=(shard,), daemon=True)
        t.start()
        producers.append(t)

    # Wait for producers to finish listing up to target_total
    for t in producers:
        t.join()

    # Wait for consumers to finish all queued work
    work_q.join()
    done_event.set()
    for t in consumers:
        t.join()

    # ---------- Summary ----------
    total_done = done_ok + done_fail
    print("\n=== Summary ===")
    print(f"Submitted:   {submitted}")
    print(f"Completed:   {total_done} (ok={done_ok}, fail={done_fail})")
    print("Per-tag counts (ok only):")
    for i, c in enumerate(group_ok, start=1):
        print(f"  Tag {i:02d}: {c}")

    if done_fail > 0 and not args.dry_run:
        print("Note: some operations failed. Consider lowering --max-workers, increasing --max-pool/timeouts, or reducing --verify-rate.")


if __name__ == "__main__":
    # Deterministic randomness unless overridden
    random.seed(os.environ.get("SEED", "0"))
    main()

