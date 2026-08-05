#!/usr/bin/env python3

import uuid

import requests
from botocore.config import Config

from common import boto_connect, create_user


USER = "post-object-security-tester"
DISPLAY_NAME = "POST Object Security Tester"
ACCESS_KEY = "LTA527CEPH826POSTOBJ"
SECRET_KEY = "ceph826postobjectsecurityregressionkey"


def presigned_post(client, bucket, key, fields):
    conditions = [{name: value} for name, value in fields.items()]
    return client.generate_presigned_post(
        Bucket=bucket,
        Key=key,
        Fields=fields,
        Conditions=conditions,
    )


def upload(client, bucket, key, fields, payload=b"payload"):
    post = presigned_post(client, bucket, key, fields)
    return requests.post(
        post["url"],
        data=post["fields"],
        files={"file": ("payload", payload, "application/octet-stream")},
        verify=False,
        timeout=30,
    )


def main():
    create_user(USER, DISPLAY_NAME, ACCESS_KEY, SECRET_KEY)
    connection = boto_connect(
        ACCESS_KEY,
        SECRET_KEY,
        Config(retries={"total_max_attempts": 1}),
    )
    client = connection.meta.client
    bucket_name = "post-object-security-" + uuid.uuid4().hex
    bucket = connection.create_bucket(Bucket=bucket_name)

    try:
        # Browser-style multipart POST must preserve bytes after an embedded
        # NUL while making every control byte safe for a response header.
        key = "mixed-null-control"
        metadata = "A\0B\rC\tD\x7fE"
        response = upload(
            client,
            bucket_name,
            key,
            {"x-amz-meta-security-test": metadata},
        )
        assert response.status_code in (200, 204), response.text

        result = client.get_object(Bucket=bucket_name, Key=key)
        expected = "=?UTF-8?Q?A=00B=0DC=09D=7FE?="
        assert result["Body"].read() == b"payload"
        assert result["Metadata"]["security-test"] == expected
        assert (
            result["ResponseMetadata"]["HTTPHeaders"]
            ["x-amz-meta-security-test"] == expected
        )

        # A NUL-only website redirect used to become an empty C string.
        redirect_key = "null-redirect"
        response = upload(
            client,
            bucket_name,
            redirect_key,
            {"x-amz-website-redirect-location": "\0"},
        )
        assert response.status_code in (200, 204), response.text
        result = client.head_object(Bucket=bucket_name, Key=redirect_key)
        assert result["WebsiteRedirectLocation"] == "=?UTF-8?Q?=00?="

        # Multipart form field names that cannot be HTTP response field names
        # must be rejected instead of becoming persistent object metadata.
        response = upload(
            client,
            bucket_name,
            "invalid-name",
            {"x-amz-meta-bad:name": "value"},
        )
        assert response.status_code == 400, response.text
    finally:
        bucket.objects.all().delete()
        bucket.delete()


if __name__ == "__main__":
    requests.packages.urllib3.disable_warnings()
    main()
