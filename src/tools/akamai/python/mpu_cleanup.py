'''
JIRA: https://track.akamai.com/jira/browse/OBJGEN1-1463

Developer: @vgoot
Code Review: @sung 

This script can detect and delete incomplete MPUs. When run in dry mode, it will report all incomplete MPUs that exceed a configurable cutoff time (default is 24 hours) for a specified bucket. When dry mode is disabled, the script will enforce the deletion of these incomplete MPUs. You can also control concurrency by adjusting the thread pool of workers.

'''
import boto3
import sys
import traceback
import time
import argparse
from datetime import datetime, timedelta, timezone
from concurrent.futures import ThreadPoolExecutor, as_completed
from botocore.client import Config

args = {
    "access_key": "",
    "secret_key": "",
    "endpoint_url": "",
}

config = Config(
    retries={
        'mode': 'standard',
        'max_attempts': 1
    }
)

session = boto3.Session(aws_access_key_id=args['access_key'], aws_secret_access_key=args['secret_key'])
s3 = session.resource('s3', endpoint_url=args['endpoint_url'], config=config)

def abort_upload(key, upload_id):
    if cmd_args.dry_run:
        return f"Dry run: {key}, UploadId: {upload_id}"

    try:
        s3.meta.client.abort_multipart_upload(
            Bucket=cmd_args.bucket,
            Key=key,
            UploadId=upload_id
        )
        return f"Aborting multipart upload for object: {key}, UploadId: {upload_id}"
    except Exception as e:
        return f"Failed to abort {key}: {e}"

def process_incomplete_mpu():
    total_uploads = 0
    expired_uploads = 0

    cutoff_date = datetime.utcnow() - timedelta(hours=cmd_args.cutoff_hours)
    cutoff_date = cutoff_date.replace(tzinfo=timezone.utc)
    try:
        paginator = s3.meta.client.get_paginator('list_multipart_uploads')
        page_iterator = paginator.paginate(Bucket=cmd_args.bucket)

        for page in page_iterator:
            uploads = page.get('Uploads', [])
            if not uploads:
                print("No in-progress multipart uploads found.")
                continue

            with ThreadPoolExecutor(max_workers=cmd_args.num_threads) as executor:
                futures = []

                for upload in uploads:
                    total_uploads += 1
                    key = upload['Key']
                    upload_id = upload['UploadId']
                    mtime = upload['Initiated']
                    if (mtime.replace(tzinfo=timezone.utc) < cutoff_date):
                        expired_uploads += 1
                        futures.append(
                            executor.submit(abort_upload, key, upload_id)
                        )

                for future in as_completed(futures):
                    result = future.result()
                    print(result)

                print("%d HEARTBEAT {total:%d, expired:%d}" % (time.time(), total_uploads, expired_uploads))
    except:
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)

    print("%d DONE {total:%d, expired:%d}" % (time.time(), total_uploads, expired_uploads))

parser = argparse.ArgumentParser()
parser.add_argument('--bucket', action="store", required=True, type=str, help='The bucket name')
parser.add_argument('--cutoff_hours', action="store", type=int, default=24, help='Cutoff hours from current time. MPU older will be scheduled for delete. Defaults to 24 hours.')
parser.add_argument('--num_threads', action="store", type=int, default=100, help='Thread pool size. Number of workers. Defaults to 100.')
parser.add_argument('--dry_run', action="store_true", help='Run in a dry mode (reporting only).')
cmd_args = parser.parse_args()

process_incomplete_mpu()
sys.exit(0)
