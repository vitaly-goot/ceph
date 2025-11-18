#!/usr/bin/env python3
import os
import sys
import time
import argparse
import threading
import queue
import signal
import logging
import faulthandler
from typing import Optional, Tuple, List
import boto3
from botocore.config import Config
from concurrent.futures import ThreadPoolExecutor, as_completed

# ---------- Logging ----------
def setup_logging(trace: bool, trace_file: Optional[str]):
    level = logging.DEBUG if trace else logging.INFO
    handler = logging.FileHandler(trace_file) if trace_file else logging.StreamHandler(sys.stdout)
    logging.basicConfig(
        level=level,
        format="%(asctime)s.%(msecs)03d [%(levelname)s] %(threadName)s %(message)s",
        datefmt="%H:%M:%S",
        handlers=[handler],
    )

def install_faulthandler():
    try:
        faulthandler.enable()
        if hasattr(signal, "SIGUSR1"):
            signal.signal(signal.SIGUSR1, lambda *args: faulthandler.dump_traceback())
    except Exception:
        pass

# ---------- Client ----------
def make_s3_client(access_key=None, secret_key=None, endpoint_url=None,
                   max_attempts=5, max_pool_connections=256, profile=None):
    access_key = access_key or os.getenv("AWS_ACCESS_KEY_ID")
    secret_key = secret_key or os.getenv("AWS_SECRET_ACCESS_KEY")
    session = boto3.Session(aws_access_key_id=access_key,
                            aws_secret_access_key=secret_key,
                            profile_name=profile)
    cfg = Config(retries={"mode": "standard", "max_attempts": max_attempts},
                 max_pool_connections=max_pool_connections)
    return session.client("s3", endpoint_url=endpoint_url, config=cfg)

# ---------- Iterators ----------
def iter_objects(s3, bucket: str, prefix: str, page_size: int = 1000):
    pag = s3.get_paginator("list_objects_v2")
    for page_i, page in enumerate(
        pag.paginate(
            Bucket=bucket,
            Prefix=prefix,
            PaginationConfig={"PageSize": page_size},
        ),
        1,
    ):
        logging.debug(
            f"[producer {prefix}] list_objects_v2 page={page_i} "
            f"keys={len(page.get('Contents', []))}"
        )
        for obj in page.get("Contents", []):
            yield (obj["Key"], None)

def iter_versions(s3, bucket: str, prefix: str, page_size: int = 1000):
    pag = s3.get_paginator("list_object_versions")
    for page_i, page in enumerate(
        pag.paginate(
            Bucket=bucket,
            Prefix=prefix,
            PaginationConfig={"PageSize": page_size},
        ),
        1,
    ):
        v = page.get("Versions", [])
        m = page.get("DeleteMarkers", [])
        logging.debug(
            f"[producer {prefix}] list_object_versions page={page_i} "
            f"versions={len(v)} markers={len(m)}"
        )
        for ent in v:
            yield (ent["Key"], ent.get("VersionId"))
        for ent in m:
            yield (ent["Key"], ent.get("VersionId"))

# ---------- Deleter ----------
def delete_batch(s3, bucket: str, items: List[Tuple[str, Optional[str]]]):
    """Delete up to 1000 items and return (deleted_count, error_count)."""
    if not items:
        return 0, 0
    objs = [{"Key": k, **({"VersionId": vid} if vid else {})} for k, vid in items]
    resp = s3.delete_objects(
        Bucket=bucket,
        Delete={"Objects": objs, "Quiet": False},  # Quiet=False so 'Deleted' is populated
    )
    errors = len(resp.get("Errors", []))
    deleted_list = resp.get("Deleted")
    if deleted_list is not None:
        deleted = len(deleted_list)
    else:
        deleted = max(0, len(items) - errors)
    return deleted, errors

# ---------- Worker ----------
def worker_loop(worker_id: int,
                s3,
                bucket: str,
                q: "queue.Queue[Tuple[str, Optional[str]]]",
                counters,
                args):
    batch: List[Tuple[str, Optional[str]]] = []
    last_flush = time.monotonic()

    def flush(reason: str):
        nonlocal batch, last_flush
        if not batch:
            return

        # If in peek mode, just drop the batch
        if args.peek > 0:
            logging.debug(
                f"[worker {worker_id}] peek mode; discarding batch of {len(batch)} keys "
                f"({reason}), no deletes."
            )
            batch = []
            last_flush = time.monotonic()
            return True

        now = time.monotonic()
        with counters["lock"]:
            # compute allowance based on remaining cap
            if args.max_delete is None:
                remaining = len(batch)
            else:
                remaining = args.max_delete - counters["deleted"]

            if remaining <= 0:
                logging.debug(f"[worker {worker_id}] cap reached before flush ({reason}); exiting.")
                counters["stop_event"].set()
                return False  # signal caller to exit

            to_send_count = min(len(batch), remaining)
            send = batch[:to_send_count]

            # mark deletion start time if this is the very first delete
            if counters["del_start"] is None:
                counters["del_start"] = now

        d, e = delete_batch(s3, bucket, send)

        now2 = time.monotonic()
        with counters["lock"]:
            counters["deleted"] += d
            counters["errors"] += e
            counters["del_end"] = now2  # last observed delete time

        logging.debug(
            f"[worker {worker_id}] {reason} flush size={len(send)} deleted={d} "
            f"errors={e} qsize={q.qsize()}"
        )

        batch = batch[to_send_count:]
        last_flush = time.monotonic()

        # if we exhausted cap with this flush, tell all workers to stop
        with counters["lock"]:
            if args.max_delete is not None and counters["deleted"] >= args.max_delete:
                logging.debug(f"[worker {worker_id}] cap reached after flush; signaling stop_event.")
                counters["stop_event"].set()
        return True

    while True:
        if counters["stop_event"].is_set():
            logging.debug(f"[worker {worker_id}] stop_event set; exiting.")
            break

        try:
            item = q.get(timeout=0.5)
        except queue.Empty:
            # Aged flush
            if batch and (time.monotonic() - last_flush) > args.batch_flush_seconds:
                if not flush("aged"):
                    break
            if counters["producer_done"].is_set() and q.empty():
                logging.debug(f"[worker {worker_id}] queue drained; exiting.")
                break
            continue

        try:
            if item == ("__STOP__", None):
                q.task_done()
                logging.debug(f"[worker {worker_id}] STOP; final flush.")
                if batch:
                    if not flush("final"):
                        break
                break

            # Normal item
            batch.append(item)
            q.task_done()

            # Full batch flush
            if len(batch) >= args.batch_size:
                if not flush("batch"):
                    break

        except Exception as ex:
            logging.exception(f"[worker {worker_id}] exception: {ex}")
            q.task_done()

# ---------- CLI ----------
def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Concurrent list and delete by prefix with cap and heartbeat.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--bucket", required=True, help="Bucket name")
    p.add_argument(
        "--prefix",
        action="append",
        required=True,
        help="Key prefix to delete. Can be specified multiple times for "
             "multiple prefixes, e.g. --prefix aa/ --prefix bb/",
    )
    p.add_argument("--include-versions", action="store_true",
                   help="Delete all versions and delete markers")
    p.add_argument(
        "--max-delete",
        type=int,
        help="Approximate upper bound on number of objects to delete across "
             "all prefixes. Because deletes happen in concurrent batches, the "
             "actual deleted count may slightly exceed this value (up to about "
             "one batch per worker in the worst case).",
    )
    p.add_argument("--workers", type=int, default=8,
                   help="Concurrent delete workers")
    p.add_argument("--batch-size", type=int, default=1000,
                   help="DeleteObjects batch size")
    p.add_argument("--batch-flush-seconds", type=float, default=2.0,
                   help="Flush partial batches after this idle time")
    p.add_argument("--queue-size", type=int, default=50000,
                   help="Key queue capacity")
    # Connection
    p.add_argument("--endpoint-url",
                   help="S3-compatible endpoint, for example http://rgw:8000")
    p.add_argument("--profile")
    p.add_argument("--access-key")
    p.add_argument("--secret-key")
    p.add_argument("--max-attempts", type=int, default=5)
    p.add_argument("--max-pool-connections", type=int, default=256)
    # Tracing and visibility
    p.add_argument("--trace", action="store_true",
                   help="Enable verbose DEBUG logs")
    p.add_argument("--trace-file", help="Write logs to file")
    p.add_argument("--heartbeat", type=float, default=0.0,
                   help="Print heartbeat every N seconds (0 disables)")
    # Safety and observability
    p.add_argument("--sample", type=int, default=0,
                   help="During normal run, print first N keys observed at INFO (global)")
    p.add_argument("--peek", type=int, default=0,
                   help="Print first N matching keys (global) and exit without deleting")
    p.add_argument("--no-strict-prefix", action="store_false", dest="strict_prefix",
                   help="Do not drop keys that do not start with their prefix client side")
    p.add_argument("--yes-i-really-mean-it", action="store_true",
                   help="Required when prefix is empty (''). Prevents accidental full-bucket deletes.")
    p.set_defaults(strict_prefix=True)
    return p.parse_args(argv)

def fmt_rate(n: int, s: Optional[float]) -> str:
    if s is None or s <= 0 or n == 0:
        return "0.00 obj/s"
    return f"{n/s:,.2f} obj/s"

def main(argv=None) -> int:
    args = parse_args(argv)
    setup_logging(args.trace, args.trace_file)
    install_faulthandler()

    s3 = make_s3_client(
        access_key=args.access_key,
        secret_key=args.secret_key,
        endpoint_url=args.endpoint_url,
        max_attempts=args.max_attempts,
        max_pool_connections=args.max_pool_connections,
        profile=args.profile,
    )

    prefixes: List[str] = args.prefix  # list due to action="append"
    if any(p == "" for p in prefixes):
        if not args.yes_i_really_mean_it:
            print("ERROR: empty prefix requires --yes-i-really-mean-it to proceed.")
            return 2

    q: "queue.Queue[Tuple[str, Optional[str]]]" = queue.Queue(maxsize=args.queue_size)
    counters = {
        "found": 0,
        "deleted": 0,
        "errors": 0,
        "sampled": 0,
        "producer_done": threading.Event(),   # all producers done
        "stop_event": threading.Event(),
        "lock": threading.Lock(),
        "producers_active": len(prefixes),
        # timing windows
        "list_start": None,
        "list_end": None,
        "del_start": None,
        "del_end": None,
    }

    # Heartbeat (INFO), only if heartbeat > 0
    def heartbeat():
        while not counters["producer_done"].is_set() or not q.empty():
            now = time.monotonic()
            with counters["lock"]:
                found = counters["found"]
                deleted = counters["deleted"]
                errors = counters["errors"]
                list_start = counters["list_start"]
                del_start = counters["del_start"]

            list_elapsed = (now - list_start) if list_start is not None else None
            del_elapsed = (now - del_start) if del_start is not None else None

            list_rps = fmt_rate(found, list_elapsed)
            del_rps = fmt_rate(deleted, del_elapsed)

            logging.info(
                f"[hb] qsize={q.qsize()} found={found} deleted={deleted} errors={errors} "
                f"| list_rps={list_rps} del_rps={del_rps}"
            )
            time.sleep(args.heartbeat)

        # final heartbeat
        now = time.monotonic()
        with counters["lock"]:
            found = counters["found"]
            deleted = counters["deleted"]
            errors = counters["errors"]
            list_start = counters["list_start"]
            del_start = counters["del_start"]
        list_elapsed = (now - list_start) if list_start is not None else None
        del_elapsed = (now - del_start) if del_start is not None else None
        list_rps = fmt_rate(found, list_elapsed)
        del_rps = fmt_rate(deleted, del_elapsed)

        logging.info(
            f"[hb] done qsize={q.qsize()} found={found} deleted={deleted} errors={errors} "
            f"| list_rps={list_rps} del_rps={del_rps}"
        )

    if args.heartbeat > 0:
        threading.Thread(target=heartbeat, name="heartbeat", daemon=True).start()

    # Producer for a single prefix
    def producer_for_prefix(prefix: str):
        start = time.monotonic()

        if args.include_versions:
            it = iter_versions(s3, args.bucket, prefix)
        else:
            it = iter_objects(s3, args.bucket, prefix)

        try:
            for k, v in it:
                # Client-side strict prefix safety for this prefix
                if args.strict_prefix and not k.startswith(prefix):
                    continue

                with counters["lock"]:
                    # If cap is already hit, stop listing
                    if args.max_delete is not None and counters["deleted"] >= args.max_delete:
                        logging.debug(f"[producer {prefix}] cap already reached; stopping listing.")
                        break

                    counters["found"] += 1
                    f = counters["found"]

                    # mark listing start time at first key
                    if counters["list_start"] is None:
                        counters["list_start"] = time.monotonic()

                    sampled = counters["sampled"]

                # Peek is global by found count
                if args.peek and f <= args.peek:
                    print(f"[peek] {f}: {k}")

                # Sample is also global
                if args.sample and sampled < args.sample:
                    with counters["lock"]:
                        if counters["sampled"] < args.sample:
                            counters["sampled"] += 1
                            sidx = counters["sampled"]
                            logging.info(f"[sample] {sidx}: {k}")

                if args.peek and f >= args.peek:
                    logging.debug(f"[producer {prefix}] peek limit reached; stopping listing.")
                    break

                # Normal enqueue
                while True:
                    try:
                        q.put((k, v), timeout=0.5)
                        break
                    except queue.Full:
                        # Backpressure; bail if cap already hit
                        if args.max_delete is not None:
                            with counters["lock"]:
                                if counters["deleted"] >= args.max_delete:
                                    logging.debug(
                                        f"[producer {prefix}] cap reached while queue full; "
                                        "stopping listing."
                                    )
                                    return time.monotonic() - start
                        continue

        finally:
            # Mark this producer as done; if it's the last one, set producer_done and enqueue STOPs
            with counters["lock"]:
                counters["producers_active"] -= 1
                remaining = counters["producers_active"]
                last = remaining == 0
                if last:
                    counters["list_end"] = time.monotonic()

            if last:
                counters["producer_done"].set()
                for _ in range(args.workers):
                    try:
                        q.put(("__STOP__", None), timeout=0.5)
                    except queue.Full:
                        # If queue full and cap hit, workers will exit via stop_event
                        pass

        return time.monotonic() - start

    t0 = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.workers + len(prefixes)) as pool:
        # Spawn workers
        t_del_start_wall = time.monotonic()
        worker_futs = [
            pool.submit(worker_loop, wid, s3, args.bucket, q, counters, args)
            for wid in range(args.workers)
        ]
        # Spawn one producer per prefix
        prod_futs = [
            pool.submit(producer_for_prefix, prefix)
            for prefix in prefixes
        ]

        # Wait for producers (wall time)
        list_times = [f.result() for f in prod_futs]
        t_list_wall = max(list_times) if list_times else 0.0

        if args.peek > 0:
            counters["stop_event"].set()
            print(
                f"[peek] listed {counters['found']} keys that match prefixes "
                f"{prefixes}. Exiting."
            )
            return 0

        # Wait for workers
        for f in as_completed(worker_futs):
            _ = f.result()
        t_delete_wall = time.monotonic() - t_del_start_wall

    t_total = time.monotonic() - t0

    # Use the precise listing/deletion windows if available
    with counters["lock"]:
        list_start = counters["list_start"]
        list_end = counters["list_end"]
        del_start = counters["del_start"]
        del_end = counters["del_end"]
        found = counters["found"]
        deleted = counters["deleted"]
        errors = counters["errors"]

    list_window = (list_end - list_start) if list_start is not None and list_end is not None else None
    del_window = (del_end - del_start) if del_start is not None and del_end is not None else None

    list_rps = fmt_rate(found, list_window)
    del_rps = fmt_rate(deleted, del_window)
    total_rps = fmt_rate(deleted, t_total)

    print(f"Bucket            : {args.bucket}")
    print(f"Prefixes          : {', '.join(prefixes)}")
    print(f"IncludeVersions   : {args.include_versions}")
    print(f"Workers           : {args.workers}")
    print(f"BatchSize         : {args.batch_size}")
    print(f"MaxDelete (approx): {args.max_delete if args.max_delete is not None else 'unlimited'}")
    print(f"Found (iterated)  : {found}")
    print(f"Deleted           : {deleted}")
    print(f"Errors            : {errors}")
    print()
    if list_window is not None:
        print(f"Listing window    : {list_window:.3f}s  | rate {list_rps}")
    else:
        print(f"Listing window    : n/a")
    if del_window is not None:
        print(f"Deletion window   : {del_window:.3f}s | rate {del_rps}")
    else:
        print(f"Deletion window   : n/a")
    print(f"Total elapsed     : {t_total:.3f}s | overall {total_rps}")

    # (Optional) you can still print the older wall-clock overlaps if you care:
    # print(f"(wall) listing    : {t_list_wall:.3f}s")
    # print(f"(wall) deletion   : {t_delete_wall:.3f}s")

    return 0 if errors == 0 else 2

if __name__ == "__main__":
    sys.exit(main())

