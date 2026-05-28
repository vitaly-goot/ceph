#!/usr/bin/env python3
"""
Repro script: "lagging MPU part + retry same part number + complete MPU while original lagging upload still in flight".

Goal:
1) Create MPU
2) Upload N parts in parallel
3) Make one part upload intentionally slow ("lagging")
4) While slow upload is still running, retry SAME PartNumber with a fast upload
5) Complete MPU using the fast retry's ETag for that PartNumber
6) Let the original slow upload finish AFTER completion (to exercise inconsistent index / phantom-part behavior)

Works against AWS S3 or S3-compatible (Ceph RGW). Whether you see a "phantom part" depends on server-side behavior/bug.

Usage example:
  python3 mpu_lag_retry_complete.py \
    --endpoint https://rgw.example.com \
    --access-key AKIA... --secret-key ... \
    --bucket my-bkt --key test/mpu-race.bin \
    --parts 6 --part-size-mb 6 \
    --lag-part 3 --lag-sleep-ms 50 --retry-after-sec 1.5 \
    --path-style
"""

import argparse
import hashlib
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import boto3
from botocore.config import Config
from botocore.exceptions import ClientError


def log(msg: str):
    ts = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
    print(f"[{ts}] {msg}", flush=True)


class SlowStream:
    """
    File-like object that sleeps between reads to slow down upload_part streaming.
    boto3/botocore reads via .read(amt).
    """
    def __init__(self, data: bytes, sleep_ms: int, chunk: int = 64 * 1024, started_evt=None):
        self._data = data
        self._sleep = sleep_ms / 1000.0
        self._chunk = chunk
        self._pos = 0
        self._lock = threading.Lock()
        self._started_evt = started_evt

    def read(self, amt: int = -1) -> bytes:
        with self._lock:
            if self._pos == 0 and self._started_evt is not None:
                self._started_evt.set()

            if self._pos >= len(self._data):
                return b""

            if amt is None or amt < 0:
                amt = len(self._data) - self._pos

            # throttle to smaller chunks to ensure we actually "lag"
            amt = min(amt, self._chunk, len(self._data) - self._pos)
            out = self._data[self._pos : self._pos + amt]
            self._pos += amt

        # sleep OUTSIDE lock so retry thread can proceed freely
        time.sleep(self._sleep)
        return out

    def tell(self) -> int:
        with self._lock:
            return self._pos

    def seek(self, offset: int, whence: int = os.SEEK_SET) -> int:
        with self._lock:
            if whence == os.SEEK_SET:
                new_pos = offset
            elif whence == os.SEEK_CUR:
                new_pos = self._pos + offset
            elif whence == os.SEEK_END:
                new_pos = len(self._data) + offset
            else:
                raise ValueError("Unsupported whence for SlowStream.seek")

            if new_pos < 0:
                raise ValueError("Negative seek position")

            self._pos = min(new_pos, len(self._data))
            return self._pos


def make_part_bytes(part_number: int, size_bytes: int) -> bytes:
    # Deterministic-ish payload per part to ease debugging
    # (Don’t use all-zero to avoid compression/dedup edge cases on some systems.)
    seed = hashlib.sha256(f"part-{part_number}".encode()).digest()
    buf = bytearray()
    while len(buf) < size_bytes:
        seed = hashlib.sha256(seed).digest()
        buf.extend(seed)
    return bytes(buf[:size_bytes])


def upload_part(client, bucket, key, upload_id, part_number, body, content_length):
    t0 = time.time()
    resp = client.upload_part(
        Bucket=bucket,
        Key=key,
        UploadId=upload_id,
        PartNumber=part_number,
        Body=body,
        ContentLength=content_length,
    )
    dt = time.time() - t0
    etag = resp["ETag"]
    return {"PartNumber": part_number, "ETag": etag, "seconds": dt}


def list_parts_safe(client, bucket, key, upload_id):
    try:
        parts = []
        paginator = client.get_paginator("list_parts")
        for page in paginator.paginate(Bucket=bucket, Key=key, UploadId=upload_id):
            parts.extend(page.get("Parts", []))
        return parts
    except ClientError as e:
        return [{"_error": str(e)}]


def list_mpu_safe(client, bucket, prefix):
    try:
        uploads = []
        paginator = client.get_paginator("list_multipart_uploads")
        for page in paginator.paginate(Bucket=bucket, Prefix=prefix):
            uploads.extend(page.get("Uploads", []))
        return uploads
    except ClientError as e:
        return [{"_error": str(e)}]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--endpoint", default=None, help="S3 endpoint URL (omit for AWS)")
    ap.add_argument("--region", default="us-east-1")
    ap.add_argument("--access-key", default=os.getenv("AWS_ACCESS_KEY_ID"))
    ap.add_argument("--secret-key", default=os.getenv("AWS_SECRET_ACCESS_KEY"))
    ap.add_argument("--session-token", default=os.getenv("AWS_SESSION_TOKEN"))
    ap.add_argument("--bucket", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--parts", type=int, default=6)
    ap.add_argument("--part-size-mb", type=int, default=6, help=">=5MB recommended for real MPU semantics")
    ap.add_argument("--lag-part", type=int, default=None, help="Optional 1-based part number to lag + retry; omit for normal uploads")
    ap.add_argument("--lag-sleep-ms", type=int, default=50, help="Sleep per read chunk for lagging part")
    ap.add_argument("--retry-after-sec", type=float, default=1.5, help="Wait before starting retry upload")
    ap.add_argument("--retry-max-attempts", type=int, default=3, help="Number of attempts for the fast retry upload")
    ap.add_argument("--retry-backoff-sec", type=float, default=1.0, help="Delay between fast retry attempts")
    ap.add_argument("--part-retry-max", type=int, default=2, help="Retries for normal (non-lag) part uploads")
    ap.add_argument("--part-retry-backoff-sec", type=float, default=1.0, help="Delay between normal part retries")
    ap.add_argument("--client-retries", type=int, default=1, help="Botocore client retry attempts (standard mode)")
    ap.add_argument("--allow-small-parts", action="store_true", help="Permit part sizes below 5MB (S3 will reject multi-part unless only one part)")
    ap.add_argument("--connect-timeout", type=float, default=5.0, help="TCP connect timeout (seconds)")
    ap.add_argument("--read-timeout", type=float, default=300.0, help="Read timeout (seconds)")
    ap.add_argument("--max-workers", type=int, default=8)
    ap.add_argument("--path-style", action="store_true", help="Use path-style addressing (often needed for RGW)")
    ap.add_argument("--insecure", action="store_true", help="Disable TLS verification")
    ap.add_argument("--cleanup", action="store_true", help="Try abort MPU if still present + delete object at end")
    args = ap.parse_args()

    if not args.access_key or not args.secret_key:
        log("Missing credentials: provide --access-key/--secret-key or set AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY")
        sys.exit(2)

    addr_style = "path" if args.path_style else "virtual"
    cfg = Config(
        region_name=args.region,
        signature_version="s3v4",
        retries={"max_attempts": args.client_retries, "mode": "standard"},
        s3={"addressing_style": addr_style},
        max_pool_connections=max(args.max_workers, 10),
        connect_timeout=args.connect_timeout,
        read_timeout=args.read_timeout,
        tcp_keepalive=True,  # keep connections alive to reduce mid-upload closes on some gateways
    )

    s = boto3.session.Session()
    client = s.client(
        "s3",
        endpoint_url=args.endpoint,
        aws_access_key_id=args.access_key,
        aws_secret_access_key=args.secret_key,
        aws_session_token=args.session_token,
        verify=(not args.insecure),
        config=cfg,
    )

    part_size = args.part_size_mb * 1024 * 1024
    lag_part = args.lag_part
    if lag_part is not None and (lag_part < 1 or lag_part > args.parts):
        log("--lag-part must be within [1, --parts]")
        sys.exit(2)

    if not args.allow_small_parts and args.parts > 1 and part_size < 5 * 1024 * 1024:
        log("Part size is below 5MB and multiple parts are requested; S3 will return EntityTooSmall. Increase --part-size-mb or pass --allow-small-parts to proceed anyway.")
        sys.exit(2)

    log(f"Starting MPU: s3://{args.bucket}/{args.key} parts={args.parts} part_size={args.part_size_mb}MB lag_part={lag_part if lag_part is not None else 'none'}")
    create = client.create_multipart_upload(Bucket=args.bucket, Key=args.key)
    upload_id = create["UploadId"]
    log(f"UploadId={upload_id}")

    # Prepare part payloads
    part_payloads = {pn: make_part_bytes(pn, part_size) for pn in range(1, args.parts + 1)}

    # Track results
    uploaded = {}         # part_number -> ETag (the one we will use for complete)
    uploaded_lock = threading.Lock()
    slow_started = threading.Event() if lag_part is not None else None

    def normal_part_task(pn: int):
        data = part_payloads[pn]
        last_error = None
        for attempt in range(1, args.part_retry_max + 1):
            try:
                r = upload_part(client, args.bucket, args.key, upload_id, pn, data, len(data))
                with uploaded_lock:
                    uploaded[pn] = r["ETag"]
                log(f"PART {pn} uploaded (normal) etag={r['ETag']} took={r['seconds']:.3f}s (attempt {attempt}/{args.part_retry_max})")
                return {"ok": True, **r}
            except Exception as e:
                last_error = e
                log(f"PART {pn} upload failed attempt {attempt}/{args.part_retry_max}: {repr(e)}")
                if attempt < args.part_retry_max:
                    time.sleep(args.part_retry_backoff_sec)
        return {"ok": False, "PartNumber": pn, "error": repr(last_error)}

    def slow_part_task(pn: int):
        data = part_payloads[pn]
        slow_body = SlowStream(data=data, sleep_ms=args.lag_sleep_ms, chunk=64 * 1024, started_evt=slow_started)
        try:
            r = upload_part(client, args.bucket, args.key, upload_id, pn, slow_body, len(data))
            # NOTE: We intentionally do NOT overwrite uploaded[pn] here.
            log(f"PART {pn} uploaded (SLOW original) etag={r['ETag']} took={r['seconds']:.3f}s")
            return {"ok": True, **r}
        except Exception as e:
            log(f"PART {pn} slow original finished with ERROR: {repr(e)}")
            return {"ok": False, "PartNumber": pn, "error": repr(e)}

    # Start uploads. If lag_part is provided, we orchestrate the slow+retry dance; otherwise everything uploads normally.
    futures = []
    slow_future = None

    with ThreadPoolExecutor(max_workers=args.max_workers) as ex:
        if lag_part is None:
            for pn in range(1, args.parts + 1):
                futures.append(ex.submit(normal_part_task, pn))
        else:
            slow_future = ex.submit(slow_part_task, lag_part)
            for pn in range(1, args.parts + 1):
                if pn == lag_part:
                    continue
                futures.append(ex.submit(normal_part_task, pn))

            # Ensure slow part has begun sending data (best effort)
            slow_started.wait(timeout=5.0)
            log(f"Lagging part {lag_part} started (best-effort={slow_started.is_set()}). Sleeping {args.retry_after_sec}s then retrying same PartNumber...")

            time.sleep(args.retry_after_sec)

            # Retry upload for SAME PartNumber with fast body
            retry_data = part_payloads[lag_part]
            retry_res = None
            for attempt in range(1, args.retry_max_attempts + 1):
                log(f"RETRY uploading PartNumber={lag_part} with FAST body now (attempt {attempt}/{args.retry_max_attempts})")
                try:
                    retry_res = upload_part(
                        client,
                        args.bucket,
                        args.key,
                        upload_id,
                        lag_part,
                        retry_data,
                        len(retry_data),
                    )
                    break
                except Exception as exc:
                    log(f"FAST retry attempt {attempt} failed: {repr(exc)}")
                    if attempt == args.retry_max_attempts:
                        log("Exhausted fast retry attempts; aborting MPU")
                        try:
                            client.abort_multipart_upload(Bucket=args.bucket, Key=args.key, UploadId=upload_id)
                            log("Aborted MPU")
                        except Exception as abort_exc:
                            log(f"Abort failed after fast retry exhaustion: {repr(abort_exc)}")
                        sys.exit(1)
                    time.sleep(args.retry_backoff_sec)

            if retry_res is None:
                log("Unexpected state: fast retry result missing; aborting")
                sys.exit(1)

            with uploaded_lock:
                uploaded[lag_part] = retry_res["ETag"]
            log(f"PART {lag_part} uploaded (FAST retry) etag={retry_res['ETag']} took={retry_res['seconds']:.3f}s")

        # Wait for all non-lag parts (or all parts when no lag) to finish
        failed_parts = []
        for f in as_completed(futures):
            try:
                res = f.result()
                if isinstance(res, dict) and not res.get("ok", True):
                    failed_parts.append(res)
                    log(f"Non-lag part failed: {res}")
            except Exception as exc:
                log(f"Non-lag part future raised: {repr(exc)}")
                try:
                    client.abort_multipart_upload(Bucket=args.bucket, Key=args.key, UploadId=upload_id)
                    log("Aborted MPU due to worker exception")
                except Exception as abort_exc:
                    log(f"Abort failed after worker exception: {repr(abort_exc)}")
                sys.exit(1)

        if failed_parts:
            log(f"Aborting MPU because {len(failed_parts)} non-lag parts failed")
            try:
                client.abort_multipart_upload(Bucket=args.bucket, Key=args.key, UploadId=upload_id)
                log("Aborted MPU due to failed parts")
            except Exception as abort_exc:
                log(f"Abort failed after part errors: {repr(abort_exc)}")
            sys.exit(1)

        # Optional visibility into server-side part listing before complete
        lp = list_parts_safe(client, args.bucket, args.key, upload_id)
        log(f"list_parts BEFORE complete: {len(lp)} entries")
        if lp and isinstance(lp[0], dict) and "_error" in lp[0]:
            log(f"list_parts error: {lp[0]['_error']}")
        else:
            # print a compact list
            compact = ", ".join([f"{p.get('PartNumber')}:{p.get('ETag')}" for p in lp if "PartNumber" in p])
            log(f"parts BEFORE complete: {compact}")

        # Complete MPU using ETags in ascending PartNumber order
        with uploaded_lock:
            missing = [pn for pn in range(1, args.parts + 1) if pn not in uploaded]
        if missing:
            log(f"ERROR: missing uploaded parts for completion: {missing} (aborting)")
            try:
                client.abort_multipart_upload(Bucket=args.bucket, Key=args.key, UploadId=upload_id)
                log("Aborted MPU")
            except Exception as e:
                log(f"Abort failed: {repr(e)}")
            sys.exit(1)

        with uploaded_lock:
            complete_parts = [{"PartNumber": pn, "ETag": uploaded[pn]} for pn in range(1, args.parts + 1)]

        log("COMPLETING MPU now (using FAST retry ETag for lagging part)")
        try:
            comp = client.complete_multipart_upload(
                Bucket=args.bucket,
                Key=args.key,
                UploadId=upload_id,
                MultipartUpload={"Parts": complete_parts},
            )
            log(f"complete_multipart_upload result: ETag={comp.get('ETag')} Location={comp.get('Location')}")
        except Exception as exc:
            log(f"complete_multipart_upload failed: {repr(exc)}")
            sys.exit(1)

        # Let the original slow upload finish AFTER completion
        if lag_part is not None:
            log("Waiting for original SLOW upload thread to finish (this is where server bugs may surface)")
            try:
                slow_result = slow_future.result() if slow_future is not None else None
            except Exception as exc:
                slow_result = {"ok": False, "error": repr(exc)}
                log(f"Slow future raised: {repr(exc)}")
            log(f"Slow original result: {slow_result}")

    # Post-checks
    try:
        head = client.head_object(Bucket=args.bucket, Key=args.key)
        log(f"head_object: ContentLength={head.get('ContentLength')} ETag={head.get('ETag')}")
    except ClientError as e:
        log(f"head_object error: {e}")

    # List MPUs to see if upload still shows up
    prefix = args.key.rsplit("/", 1)[0] + "/" if "/" in args.key else ""
    uploads = list_mpu_safe(client, args.bucket, prefix)
    still_there = [u for u in uploads if isinstance(u, dict) and u.get("UploadId") == upload_id]
    log(f"list_multipart_uploads(prefix='{prefix}') returned {len(uploads)} uploads; this UploadId present? {bool(still_there)}")

    if args.cleanup:
        log("Cleanup enabled: attempting abort MPU (no-op if already complete) + delete object")
        try:
            client.abort_multipart_upload(Bucket=args.bucket, Key=args.key, UploadId=upload_id)
            log("abort_multipart_upload attempted")
        except ClientError as e:
            log(f"abort_multipart_upload error (often expected after complete): {e}")
        try:
            client.delete_object(Bucket=args.bucket, Key=args.key)
            log("delete_object attempted")
        except ClientError as e:
            log(f"delete_object error: {e}")

    log("Done.")


if __name__ == "__main__":
    main()
