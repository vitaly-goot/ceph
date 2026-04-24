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
import random
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
    # comment this if you would like to get boto traces 
    for noisy in ("botocore", "boto3", "urllib3"):
        logging.getLogger(noisy).setLevel(logging.INFO) 

def install_faulthandler():
    try:
        faulthandler.enable()
        if hasattr(signal, "SIGUSR1"):
            signal.signal(signal.SIGUSR1, lambda *args: faulthandler.dump_traceback())
    except Exception:
        pass

# ---------- Client ----------
def make_s3_client(access_key=None, secret_key=None, endpoint_url=None,
                   max_attempts=5, max_pool_connections=256, profile=None,
                   connect_timeout=5, read_timeout=5):
    access_key = access_key or os.getenv("AWS_ACCESS_KEY_ID")
    secret_key = secret_key or os.getenv("AWS_SECRET_ACCESS_KEY")
    session = boto3.Session(aws_access_key_id=access_key,
                            aws_secret_access_key=secret_key,
                            profile_name=profile)
    cfg = Config(
        retries={"mode": "standard", "max_attempts": max_attempts},
        max_pool_connections=max_pool_connections,
        connect_timeout=connect_timeout,
        read_timeout=read_timeout,
    )
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
        yield [(obj["Key"], None) for obj in page.get("Contents", [])]

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
        page_entries: List[Tuple[str, Optional[str]]] = []
        for ent in v:
            page_entries.append((ent["Key"], ent.get("VersionId")))
        for ent in m:
            page_entries.append((ent["Key"], ent.get("VersionId")))
        yield page_entries

# ---------- Deleter ----------
def delete_one_by_one(s3, bucket: str, items: List[Tuple[str, Optional[str]]]):
    """Fallback: delete items individually when bulk delete fails."""
    deleted = 0
    errors = 0
    for k, vid in items:
        try:
            if vid:
                s3.delete_object(Bucket=bucket, Key=k, VersionId=vid)
            else:
                s3.delete_object(Bucket=bucket, Key=k)
            deleted += 1
        except Exception as ex:
            logging.warning("delete_object failed for %s: %s", k, ex)
            errors += 1
    return deleted, errors

def delete_batch(s3, bucket: str, items: List[Tuple[str, Optional[str]]], use_individual: bool = False):
    """Delete up to 1000 items and return (deleted_count, error_count)."""
    if not items:
        return 0, 0
    
    if use_individual:
        return delete_one_by_one(s3, bucket, items)
    
    objs = [{"Key": k, **({"VersionId": vid} if vid else {})} for k, vid in items]
    sample_preview = ", ".join(k for k, _ in items[:3])
    logging.debug("delete_batch request size=%s sample=%s", len(items), sample_preview)
    
    t_start = time.monotonic()
    try:
        resp = s3.delete_objects(
            Bucket=bucket,
            Delete={"Objects": objs, "Quiet": False},  # Quiet=False so 'Deleted' is populated
        )
        t_elapsed = time.monotonic() - t_start
        logging.debug("delete_batch response received in %.3fs", t_elapsed)
    except Exception as ex:
        t_elapsed = time.monotonic() - t_start
        logging.info("delete_batch bulk request failed after %.3fs: %s; falling back to individual deletes", t_elapsed, ex)
        return delete_one_by_one(s3, bucket, items)

    errors_list = resp.get("Errors") or []
    deleted_list = resp.get("Deleted") or []

    errors = len(errors_list)

    if deleted_list:
        deleted = len(deleted_list)
    else:
        # Some S3-compatible endpoints (e.g. older RGW releases) omit the
        # 'Deleted' field even when the request succeeds. Fall back to the
        # request size minus the reported errors so counters still advance.
        deleted = max(0, len(items) - errors)

        if deleted and not errors:
            logging.debug("delete_objects response lacked 'Deleted'; inferred %s successes", deleted)

    if errors_list:
        logging.warning("delete_objects reported %s errors: %s", errors, errors_list[:3])

    logging.debug(
        "delete_batch response deleted=%s errors=%s sample=%s",
        deleted,
        errors,
        sample_preview,
    )

    return deleted, errors


class CounterState:
    def __init__(self, producers_active: int):
        self._lock = threading.Lock()
        self.producer_done = threading.Event()
        self.stop_event = threading.Event()
        self.producers_active = producers_active
        self.found = 0
        self.deleted = 0
        self.errors = 0
        self.sampled = 0
        self.list_start: Optional[float] = None
        self.list_end: Optional[float] = None
        self.del_start: Optional[float] = None
        self.del_end: Optional[float] = None
        # Tracks number of keys reserved for deletion but not yet reflected
        # in deleted/errors counters (in-flight). Used for strict cap.
        self.reserved = 0

    def snapshot(self):
        with self._lock:
            return {
                "found": self.found,
                "deleted": self.deleted,
                "errors": self.errors,
                "sampled": self.sampled,
                "list_start": self.list_start,
                "list_end": self.list_end,
                "del_start": self.del_start,
                "del_end": self.del_end,
                "reserved": self.reserved,
            }

    def should_stop(self) -> bool:
        return self.stop_event.is_set()

    def increment_found(self, now: float) -> Tuple[int, int]:
        with self._lock:
            if self.list_start is None:
                self.list_start = now
            self.found += 1
            return self.found, self.sampled

    def try_sample(self, limit: int) -> Optional[int]:
        if limit <= 0:
            return None
        with self._lock:
            if self.sampled >= limit:
                return None
            self.sampled += 1
            return self.sampled

    def deleted_reached(self, cap: Optional[int]) -> bool:
        if cap is None:
            return False
        with self._lock:
            return self.deleted >= cap

    def reserve_delete_batch(self, batch_len: int, cap: Optional[int], now: float) -> int:
        if batch_len <= 0:
            return 0
        with self._lock:
            if self.del_start is None:
                self.del_start = now
            if cap is None:
                return batch_len
            # Strict remaining considers in-flight reservations.
            remaining = cap - (self.deleted + self.reserved)
            if remaining <= 0:
                logging.debug("reserve_delete_batch cap exhausted; refusing batch")
                self.stop_event.set()
                return 0
            allowed = batch_len if batch_len <= remaining else remaining
            if allowed < batch_len:
                logging.debug(
                    "reserve_delete_batch truncating batch batch=%s allowed=%s remaining(before)=%s; strict cap",
                    batch_len,
                    allowed,
                    remaining,
                )
            self.reserved += allowed
            # If we've now fully reserved up to cap, signal stop.
            if self.deleted + self.reserved >= cap:
                self.stop_event.set()
            return allowed

    def record_delete_results(self, deleted: int, errors: int, finished_at: float, cap: Optional[int]):
        with self._lock:
            self.deleted += deleted
            self.errors += errors
            if deleted or errors:
                self.del_end = finished_at
            # Reduce reserved by attempted keys (deleted + errors) to reflect completion.
            attempted = deleted + errors
            if attempted > 0:
                self.reserved = max(0, self.reserved - attempted)
            if cap is not None and self.deleted >= cap:
                self.stop_event.set()

    def mark_producer_complete(self, finished_at: float) -> bool:
        with self._lock:
            self.producers_active -= 1
            if self.producers_active == 0:
                self.list_end = finished_at
                self.producer_done.set()
                return True
            return False

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
        allowed = counters.reserve_delete_batch(len(batch), args.max_delete, now)
        allowed = min(allowed, len(batch))
        if allowed <= 0:
            logging.debug(f"[worker {worker_id}] no allowance for flush ({reason}); dropping batch.")
            batch = []
            last_flush = time.monotonic()
            # Don't exit - continue draining queue in case there are STOP sentinels
            return True

        send = batch[:allowed]
        sample_keys = ", ".join(k for k, _ in send[:3])
        logging.debug(
            f"[worker {worker_id}] {reason} flush size={len(send)} sample={sample_keys}"
        )

        d, e = delete_batch(s3, bucket, send, use_individual=args.use_individual_deletes)

        finished = time.monotonic()
        counters.record_delete_results(d, e, finished, args.max_delete)

        logging.debug(
            f"[worker {worker_id}] {reason} flush deleted={d} errors={e} qsize={q.qsize()}"
        )

        discarded = len(batch) - allowed
        batch = batch[allowed:]
        if discarded > 0:
            logging.debug(f"[worker {worker_id}] discarded {discarded} keys due to strict cap enforcement")
        last_flush = finished

        if counters.should_stop():
            logging.debug(f"[worker {worker_id}] stop requested post-flush; draining queue then exiting.")
        return True

    while True:
        # Only check stop_event if queue is empty and producer is done
        if counters.should_stop() and q.empty() and counters.producer_done.is_set():
            logging.debug(f"[worker {worker_id}] stop_event set and queue drained; flushing final batch and exiting.")
            if batch:
                flush("stop-signal")
            break

        try:
            item = q.get(timeout=0.5)
        except queue.Empty:
            if (
                args.skip_page_percent == 0.0
                and args.batch_flush_seconds > 0.0
                and batch
                and (time.monotonic() - last_flush) > args.batch_flush_seconds
            ):
                if not flush("aged"):
                    break
            if counters.producer_done.is_set() and q.empty():
                if batch:
                    flush("drain")
                logging.debug(f"[worker {worker_id}] queue drained; exiting.")
                break
            continue

        try:
            if item == ("__STOP__", None):
                q.task_done()
                logging.debug(f"[worker {worker_id}] STOP received.")
                if batch:
                    flush("stop-sentinel")
                if counters.producer_done.is_set() and q.empty():
                    logging.debug(f"[worker {worker_id}] producer done and queue empty after STOP; exiting.")
                    break
                continue

            # If strict cap reached, drop further normal items immediately
            if counters.should_stop():
                q.task_done()
                logging.debug(f"[worker {worker_id}] dropping key due to strict cap (stop_event set)")
                # Drain until STOP sentinels arrive
                continue

            # Normal item
            batch.append(item)
            q.task_done()

            # Full batch flush
            if len(batch) >= args.batch_size:
                flush("batch")

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
    p.add_argument("--batch-flush-seconds", type=float, default=0.0,
                   help="Flush partial batches after this idle time (0 disables)")
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
    p.add_argument("--connect-timeout", type=int, default=5,
                   help="S3 client connect timeout in seconds")
    p.add_argument("--read-timeout", type=int, default=5,
                   help="S3 client read timeout in seconds")
    p.add_argument("--use-individual-deletes", action="store_true",
                   help="Use individual delete_object calls instead of bulk delete_objects")
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
    p.add_argument("--skip-page-percent", type=float, default=0.0,
                   help="Randomly skip this percentage of keys from each listed page before enqueueing")
    p.add_argument("--yes-i-really-mean-it", action="store_true",
                   help="Required when prefix is empty (''). Prevents accidental full-bucket deletes.")
    args = p.parse_args(argv)

    # Validate worker count: must be in [1, 256]
    if args.workers <= 0:
        p.error("--workers must be >= 1; 0 would leave no delete workers to process the queue")
    if args.workers > 256:
        p.error("--workers must be <= 256; pick a smaller value to avoid resource exhaustion")

    if args.batch_size <= 0:
        p.error("--batch-size must be >= 1")
    if args.batch_size > 1000:
        p.error("--batch-size must be <= 1000 (DeleteObjects limit)")

    return args

def fmt_rate(n: int, s: Optional[float]) -> str:
    if s is None or s <= 0 or n == 0:
        return "0.00 obj/s"
    return f"{n/s:,.2f} obj/s"

def main(argv=None) -> int:
    args = parse_args(argv)
    setup_logging(args.trace, args.trace_file)
    install_faulthandler()

    if args.max_pool_connections <= 0:
        logging.error("--max-pool-connections must be >= 1")
        return 2

    if args.max_pool_connections < args.workers:
        logging.warning(
            "max_pool_connections (%s) < workers (%s); bumping max_pool_connections to %s",
            args.max_pool_connections,
            args.workers,
            args.workers,
        )
        args.max_pool_connections = args.workers

    s3 = make_s3_client(
        access_key=args.access_key,
        secret_key=args.secret_key,
        endpoint_url=args.endpoint_url,
        max_attempts=args.max_attempts,
        max_pool_connections=args.max_pool_connections,
        profile=args.profile,
        connect_timeout=args.connect_timeout,
        read_timeout=args.read_timeout,
    )

    prefixes: List[str] = args.prefix  # list due to action="append"
    if args.skip_page_percent < 0.0 or args.skip_page_percent > 100.0:
        print("ERROR: --skip-page-percent must be between 0 and 100.")
        return 2

    if any(p == "" for p in prefixes):
        if not args.yes_i_really_mean_it:
            print("ERROR: empty prefix requires --yes-i-really-mean-it to proceed.")
            return 2

    q: "queue.Queue[Tuple[str, Optional[str]]]" = queue.Queue(maxsize=args.queue_size)
    counters = CounterState(len(prefixes))

    # Heartbeat (INFO), only if heartbeat > 0
    def heartbeat():
        while not counters.producer_done.is_set() or not q.empty():
            now = time.monotonic()
            snap = counters.snapshot()
            found = snap["found"]
            deleted = snap["deleted"]
            errors = snap["errors"]
            list_start = snap["list_start"]
            del_start = snap["del_start"]

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
        snap = counters.snapshot()
        found = snap["found"]
        deleted = snap["deleted"]
        errors = snap["errors"]
        list_start = snap["list_start"]
        del_start = snap["del_start"]
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
            page_iter = iter_versions(s3, args.bucket, prefix)
        else:
            page_iter = iter_objects(s3, args.bucket, prefix)

        try:
            stop_listing = False
            for page_index, page in enumerate(page_iter, 1):
                if counters.should_stop() or counters.deleted_reached(args.max_delete):
                    logging.debug(f"[producer {prefix}] stop requested before page {page_index}; exiting.")
                    break

                filtered_page: List[Tuple[str, Optional[str]]] = []
                for k, v in page:
                    filtered_page.append((k, v))

                if args.skip_page_percent > 0.0 and filtered_page:
                    total_before = len(filtered_page)
                    skip_count = int(total_before * args.skip_page_percent / 100)
                    if skip_count >= total_before:
                        logging.debug(
                            f"[producer {prefix}] skip-page-percent removed entire page {page_index} "
                            f"({total_before} keys)"
                        )
                        filtered_page = []
                    elif skip_count > 0:
                        skipped_indexes = set(random.sample(range(len(filtered_page)), skip_count))
                        filtered_page = [item for idx, item in enumerate(filtered_page) if idx not in skipped_indexes]
                        logging.debug(
                            f"[producer {prefix}] skip-page-percent removed {skip_count}/{total_before} keys "
                            f"on page {page_index}"
                        )

                for k, v in filtered_page:
                    if counters.should_stop():
                        logging.debug(f"[producer {prefix}] stop requested; stopping listing.")
                        stop_listing = True
                        break

                    if counters.deleted_reached(args.max_delete):
                        logging.debug(f"[producer {prefix}] cap already reached; stopping listing.")
                        stop_listing = True
                        break

                    now = time.monotonic()
                    f, sampled_before = counters.increment_found(now)

                    if args.peek and f <= args.peek:
                        print(f"[peek] {f}: {k}")

                    if args.sample and sampled_before < args.sample:
                        sidx = counters.try_sample(args.sample)
                        if sidx is not None:
                            logging.info(f"[sample] {sidx}: {k}")

                    if args.peek and f >= args.peek:
                        logging.debug(f"[producer {prefix}] peek limit reached; stopping listing.")
                        stop_listing = True
                        break

                    while True:
                        if counters.should_stop():
                            logging.debug(f"[producer {prefix}] stop requested before enqueue; exiting.")
                            return time.monotonic() - start
                        try:
                            q.put((k, v), timeout=0.5)
                            break
                        except queue.Full:
                            if counters.should_stop() or counters.deleted_reached(args.max_delete):
                                logging.debug(
                                    f"[producer {prefix}] stopping due to cap/backpressure.")
                                return time.monotonic() - start
                            continue

                if stop_listing:
                    break

        finally:
            # Mark this producer as done; enqueue STOP sentinels if it is the last one
            finished = time.monotonic()
            last = counters.mark_producer_complete(finished)

            if last:
                for _ in range(args.workers):
                    try:
                        q.put(("__STOP__", None), timeout=0.5)
                    except queue.Full:
                        # If queue full and cap hit, workers will exit once backpressure clears
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
            counters.stop_event.set()
            snap = counters.snapshot()
            print(
                f"[peek] listed {snap['found']} keys that match prefixes "
                f"{prefixes}. Exiting."
            )
            return 0

        # Wait for workers
        for f in as_completed(worker_futs):
            _ = f.result()
        t_delete_wall = time.monotonic() - t_del_start_wall

    t_total = time.monotonic() - t0

    # Use the precise listing/deletion windows if available
    snap = counters.snapshot()
    list_start = snap["list_start"]
    list_end = snap["list_end"]
    del_start = snap["del_start"]
    del_end = snap["del_end"]
    found = snap["found"]
    deleted = snap["deleted"]
    errors = snap["errors"]

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

