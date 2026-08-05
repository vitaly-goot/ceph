# SQA Test Plan — CEPH-826: CRLF Injection / HTTP Response Splitting in RGW

| Field | Value |
|-------|-------|
| Related ticket | CEPH-826 |
| Weakness class | CWE-113 (Improper Neutralization of CRLF Sequences in HTTP Headers); related CWE-93 |
| Component | Ceph RADOS Gateway (RGW), S3 and Swift front ends |
| Target versions | aka_version_20.2.2 and the aka_version_18.2.7 backport |
| Fix branch | <code>bugfix/CEPH-826-security-rgw-crlf-injection-http-response-splitting-in-s3-post-object-metadata</code> |

---

## 1. Purpose

Validate that user-controlled metadata submitted through the S3 POST Object
(browser multipart form upload) path cannot inject CR/LF or other forbidden
bytes into RGW HTTP response headers. Also validate the length-aware embedded-NUL
fix, strict HTTP field-name validation, redirect paths, and defense-in-depth
handling of objects poisoned before the fix.

## 2. Scope

### In scope

- Ingress encoding of <code>x-amz-meta-*</code> and
  <code>x-amz-website-redirect-location</code> multipart fields.
- Ingress rejection of metadata names that are not ASCII RFC 9110
  <code>field-name</code> tokens.
- Egress omission of invalid field names and replacement sanitization of
  forbidden field-value bytes in the common <code>dump_header()</code> path.
- Form-controlled <code>success_action_redirect</code> output.
- Website <code>Location</code> output from a pre-existing poisoned redirect
  attribute.
- PUT, COPY, Swift, normal redirects, and clean response-header regression.
- Byte-exact C0, DEL, embedded-NUL, HTAB, and field-name unit coverage.

## 3. References

| Ref | Item |
|-----|------|
| R1 | <code>src/rgw/rgw_rest_s3.cc</code> — POST Object metadata, website redirect, and success-action redirect paths |
| R2 | <code>src/rgw/rgw_xattr.h</code> and <code>src/common/mime.[ch]</code> — length-aware <code>format_xattr()</code> and quoted-printable encoding |
| R3 | <code>src/rgw/rgw_http_fields.h</code>, <code>src/rgw/rgw_op.h</code>, and <code>src/rgw/rgw_rest.cc</code> — field-name validation and field-value egress sanitization |
| R4 | <code>src/test/mime.cc</code> — executable byte-exact unit coverage |
| R5 | <code>qa/workunits/rgw/test_rgw_post_object_security.py</code> — automated multipart regression workunit |
| R6 | RFC 9110 §§5.5 and 5.6.2 — field values and <code>tchar</code> field names |
| R7 | RFC 2047 — MIME encoded-word representation used by <code>format_xattr()</code> |
| R8 | CWE-113 / CWE-93 |

## 4. Change overview

The fix has three independent layers:

1. **POST ingress value encoding (R1/R2).** POST metadata and website-redirect
   values are passed through <code>format_xattr()</code>, matching PUT/COPY.
   The encoder is length-aware, so an embedded NUL becomes <code>=00</code>
   instead of truncating everything after it. Every control byte, including
   HTAB, is encoded in this ingress path.

2. **Field-name validation (R3).** Metadata names must be nonempty ASCII RFC
   <code>tchar</code> tokens. Invalid multipart/request names are rejected.
   A poisoned invalid name encountered during response generation is omitted
   and logged; it is never rewritten into a different, potentially ambiguous
   header name.

3. **Egress field-value sanitization (R3).** The common
   <code>dump_header()</code> path replaces every C0 byte except HTAB, plus
   DEL, with SP. HTAB is legal in an HTTP field value and remains unchanged
   only when exercising this egress-only defense. Clean values use a view over
   the original bytes without an allocation.

Expected layer interaction:

- Newly uploaded dirty values are encoded at ingress, including
  <code>NUL → =00</code> and <code>HTAB → =09</code>.
- Pre-fix poisoned values are neutralized at egress.
- Invalid names are rejected at ingress or omitted at egress.

## 5. Test environment and tooling

### Environment

- RGW built from the fix branch.
- A pre-fix RGW.

### Tooling

| Tool | Purpose |
|------|---------|
| boto3 | Presigned POST and GET operations |
| Python socket and ssl | Complete byte-level HTTP response capture |
| curl | Supplemental functional checks only |
| radosgw-admin and rados | Locate or construct pre-existing poisoned attributes |
| <code>unittest_mime</code> | Byte-exact encoder and HTTP-field helper tests |

> A raw-wire test passes only after it proves that the intended authenticated
> operation succeeded, requires the expected metadata header and object body,
> and confirms there is no independent injected line such as
> <code>\r\nX-Injected:</code>. Searching for the token
> <code>X-Injected</code> anywhere is incorrect because a safe encoded or
> space-neutralized value may legitimately retain that printable text.

## 6. Test data

Apply the payloads as <code>x-amz-meta-foo</code> values and, where called out,
as redirect values.

| ID | Payload (C notation) | Intent |
|----|----------------------|--------|
| P1 | <code>bar\r\nX-Injected: 1</code> | Inject a complete extra header |
| P2 | <code>bar\r\n\r\n&lt;html&gt;pwned&lt;/html&gt;</code> | Split the response and inject a body |
| P3 | <code>bar\rX-Injected: 1</code> | Bare CR |
| P4 | <code>bar\nX-Injected: 1</code> | Bare LF |
| P5 | <code>bar\r\nSet-Cookie: s=evil; Domain=.victim</code> | Cookie injection |
| P6 | <code>bar\x00baz</code> | Embedded-NUL truncation |
| P7 | <code>bar\x0bbaz</code> / <code>bar\x0cbaz</code> | Vertical tab / form feed |
| P8 | <code>bar\x7fbaz</code> | DEL |
| P9 | <code>a\r\nb\r\nc\r\nd</code> | Multiple injections |
| P10 | <code>\r\nX-Injected: 1</code> | Forbidden bytes at the start |
| P11 | <code>bar\r\n</code> | Forbidden bytes at the end |
| P12 | <code>héllo wörld</code> | Clean UTF-8 negative control |
| P13 | <code>plain value with spaces</code> | Clean ASCII/SP negative control |
| P14 | <code>plain value\twith tab</code> | HTAB control input; POST/PUT must encode it as <code>=09</code> |

## 7. Detailed test cases

### 7.1 POST ingress: metadata values and names

| ID | Title | Steps | Expected result | Pri |
|----|-------|-------|-----------------|-----|
| SEC-01 | CRLF metadata is encoded | Presigned multipart POST with P1; presigned raw GET and HEAD. | POST succeeds; GET is 200; body and <code>x-amz-meta-foo</code> are present; value is <code>=?UTF-8?Q?bar=0D=0AX-Injected: 1?=</code>; no <code>\r\nX-Injected:</code> line exists. | P0 |
| SEC-02 | Response-body split is blocked | Repeat SEC-01 with P2. | One well-formed response and one header block; expected object body is returned. | P0 |
| SEC-03 | Bare CR and LF | Repeat with P3 and P4. | Both bytes are encoded; no new line is created. | P0 |
| SEC-04 | Cookie injection | Repeat with P5. | No independent <code>Set-Cookie</code> field is emitted. | P0 |
| SEC-05 | NUL, other controls, DEL, and HTAB | Repeat with P6, P7, P8, and P14. | No truncation or folding. Exact returned values contain <code>=00</code>, <code>=0B</code>/<code>=0C</code>, <code>=7F</code>, and <code>=09</code>, respectively. | P0 |
| SEC-06 | Multiple and positional injections | Repeat with P9, P10, and P11. | Every forbidden byte is encoded at every position. | P1 |
| SEC-07 | Invalid metadata field names | POST names containing colon, SP, HTAB, non-ASCII, or other non-<code>tchar</code> bytes. | HTTP 400; object is not persisted; no malformed response header is emitted. | P0 |

### 7.2 Redirect paths

| ID | Title | Steps | Expected result | Pri |
|----|-------|-------|-----------------|-----|
| SEC-10 | POST website redirect metadata | Presigned POST <code>x-amz-website-redirect-location</code> with P1/P2/P6; authenticated GET/HEAD through the S3 endpoint. | Stored/returned attribute is encoded; NUL is <code>=00</code>; no split or truncation. | P0 |
| SEC-11 | Legitimate website redirect | POST <code>/other-page</code>, make the test object readable, and GET it through the website endpoint. | HTTP 301 with exactly one valid <code>Location: /other-page</code>. | P1 |
| SEC-12 | Form-controlled <code>success_action_redirect</code> | Presign a POST policy that permits the field, submit P1 in its URL, and capture the complete POST response. | HTTP 303; exactly one <code>Location</code>; CR/LF are neutralized as SP; no <code>\r\nX-Injected:</code> line. | P0 |
| SEC-13 | Pre-existing poisoned website redirect | Create a public website object whose stored redirect attribute contains raw P1 using a pre-fix build or controlled xattr injection; serve it with fixed RGW. | HTTP 301; exactly one sanitized <code>Location</code>; printable text may remain in its value, but no independent injected header exists. | P0 |

### 7.3 Egress defense in depth

| ID | Title | Steps | Expected result | Pri |
|----|-------|-------|-----------------|-----|
| SEC-20 | Pre-fix poisoned metadata value | Store raw P1 before the fix; presigned GET with fixed RGW. | GET 200, expected object body and metadata field present; CR/LF become SP; no split. | P0 |
| SEC-21 | Swift metadata echo path | Inject or store a poisoned Swift metadata value and read it through fixed RGW. | Same value sanitization as S3. | P1 |
| SEC-22 | GET/HEAD parity | Repeat SEC-20 with HEAD. | Same sanitized metadata value. | P1 |
| SEC-23 | HTAB in a poisoned value | Inject a pre-existing raw HTAB in a field value, bypassing <code>format_xattr()</code>. | Literal HTAB remains in the value; no folding or extra field. This expectation applies only to egress-only poisoned data. | P1 |
| SEC-24 | Pre-existing poisoned field name | Inject metadata whose emitted name contains colon, whitespace, control, or non-ASCII bytes. | Field is omitted and an error is logged; valid neighboring metadata remains present. | P0 |

### 7.4 Functional regression

| ID | Title | Steps | Expected result | Pri |
|----|-------|-------|-----------------|-----|
| REG-01 | Clean POST metadata | POST P12 and P13. | Values round-trip unchanged. | P0 |
| REG-02 | POST/PUT parity | Store clean P12/P13 and dirty P1/P6/P14 through both methods. | Representations match; dirty HTAB is <code>=09</code>, not a literal tab. | P0 |
| REG-03 | Multiple metadata fields | POST several valid <code>x-amz-meta-*</code> fields. | All fields and values return correctly. | P1 |
| REG-04 | Empty and boundary values | POST an empty value and maximum supported value. | No error or crash; documented limits apply. | P2 |
| REG-05 | Unrelated headers | Exercise Content-Type, ETag, Last-Modified, request ID, CORS, Content-Range, and clean redirects. | Clean output is unchanged. | P0 |
| REG-06 | CORS | Exercise legitimate origins and methods. | No behavior change. | P1 |
| REG-07 | Existing S3 and Swift suites | Run the standard RGW functional suites. | No new failures versus baseline. | P0 |

### 7.5 Executable unit coverage

| ID | Assertion |
|----|-----------|
| UNIT-01 | Length-aware quoted-printable output is byte-exact for every C0 byte and DEL. |
| UNIT-02 | Mixed <code>A\0B\rC</code> becomes <code>=?UTF-8?Q?A=00B=0DC?=</code> without truncation. |
| UNIT-03 | NUL-only website redirect input becomes <code>=?UTF-8?Q?=00?=</code>. |
| UNIT-04 | Field-name validation accepts exactly ASCII RFC <code>tchar</code> and rejects empty/invalid names. |
| UNIT-05 | Field-value sanitization replaces every forbidden C0 byte and DEL while preserving HTAB. |
| UNIT-06 | Clean field values use the original backing bytes; dirty output preserves length. |

Run:

~~~bash
cd build
ninja unittest_mime
./bin/unittest_mime
~~~

### 7.6 Performance and interoperability

| ID | Title | Expected |
|----|-------|----------|
| PERF-01 | GET-heavy throughput and p99 latency | Difference remains within measurement noise. |
| INT-01 | boto3/AWS SDK presigned POST | Browser-style uploads succeed. |
| INT-02 | Common HTTP clients | All payload responses parse as one well-formed response. |

## 8. Raw-wire reproduction recipes

### 8.1 Presigned POST and presigned GET

This recipe uses the exact URL returned in <code>post["url"]</code>, reads each
response until EOF, asserts POST 200 and GET 200, checks the object body and
metadata header, and looks only for an independent injected line.

Set <code>RGW_ENDPOINT</code>, <code>RGW_ACCESS_KEY</code>,
<code>RGW_SECRET_KEY</code>, and optionally <code>RGW_BUCKET</code>.

~~~python
import os
import socket
import ssl
from urllib.parse import urlsplit

import boto3

endpoint = os.environ["RGW_ENDPOINT"]
access_key = os.environ["RGW_ACCESS_KEY"]
secret_key = os.environ["RGW_SECRET_KEY"]
bucket = os.environ.get("RGW_BUCKET", "ceph-826-raw-wire")
key = "poc.txt"
boundary = "----cephqa826"

s3 = boto3.client(
    "s3",
    endpoint_url=endpoint,
    aws_access_key_id=access_key,
    aws_secret_access_key=secret_key,
)


def raw_request(url, method, body=b"", headers=None):
    parsed = urlsplit(url)
    assert parsed.scheme in ("http", "https")
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    target = parsed.path or "/"
    if parsed.query:
        target += "?" + parsed.query

    sock = socket.create_connection((parsed.hostname, port), timeout=30)
    if parsed.scheme == "https":
        context = ssl.create_default_context()
        sock = context.wrap_socket(sock, server_hostname=parsed.hostname)

    request_headers = {
        "Host": parsed.netloc,
        "Connection": "close",
    }
    request_headers.update(headers or {})
    if body or method == "POST":
        request_headers["Content-Length"] = str(len(body))

    request = [f"{method} {target} HTTP/1.1\r\n".encode("ascii")]
    request.extend(
        f"{name}: {value}\r\n".encode("latin-1")
        for name, value in request_headers.items()
    )
    request.append(b"\r\n")
    request.append(body)
    sock.sendall(b"".join(request))

    response = bytearray()
    while True:
        chunk = sock.recv(65535)
        if not chunk:
            break
        response.extend(chunk)
    sock.close()
    return bytes(response)


def split_response(raw):
    header_block, marker, body = raw.partition(b"\r\n\r\n")
    assert marker, repr(raw)
    status_line, *lines = header_block.split(b"\r\n")
    status = int(status_line.split(b" ", 2)[1])
    fields = []
    for line in lines:
        name, colon, value = line.partition(b":")
        assert colon == b":", repr(line)
        fields.append((name.lower(), value.lstrip()))
    return status, fields, body


def field_values(fields, name):
    wanted = name.lower().encode("ascii")
    return [value for field_name, value in fields if field_name == wanted]


def multipart_body(fields, payload):
    parts = []
    for name, value in fields.items():
        parts.extend([
            f"--{boundary}\r\n".encode(),
            f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode(),
            str(value).encode("utf-8"),
            b"\r\n",
        ])
    parts.extend([
        f"--{boundary}\r\n".encode(),
        b'Content-Disposition: form-data; name="file"; filename="poc.txt"\r\n',
        b"Content-Type: text/plain\r\n\r\n",
        payload,
        b"\r\n",
        f"--{boundary}--\r\n".encode(),
    ])
    return b"".join(parts)


try:
    s3.create_bucket(Bucket=bucket)
except s3.exceptions.BucketAlreadyOwnedByYou:
    pass

post = s3.generate_presigned_post(
    Bucket=bucket,
    Key=key,
    Fields={"success_action_status": "200"},
    Conditions=[
        {"success_action_status": "200"},
        ["starts-with", "$x-amz-meta-foo", ""],
    ],
    ExpiresIn=3600,
)
fields = dict(post["fields"])
fields["x-amz-meta-foo"] = "bar\r\nX-Injected: 1"
post_body = multipart_body(fields, b"hello")

post_raw = raw_request(
    post["url"],
    "POST",
    post_body,
    {"Content-Type": f"multipart/form-data; boundary={boundary}"},
)
post_status, _, _ = split_response(post_raw)
assert post_status == 200, repr(post_raw)

get_url = s3.generate_presigned_url(
    "get_object",
    Params={"Bucket": bucket, "Key": key},
    ExpiresIn=3600,
)
get_raw = raw_request(get_url, "GET")
get_status, get_fields, get_body = split_response(get_raw)

expected = b"=?UTF-8?Q?bar=0D=0AX-Injected: 1?="
assert get_status == 200, repr(get_raw)
assert get_body == b"hello", repr(get_body)
assert field_values(get_fields, "content-length") == [b"5"]
assert field_values(get_fields, "x-amz-meta-foo") == [expected]
assert b"\r\nX-Injected:" not in get_raw
print("CEPH-826 raw-wire metadata check passed")
~~~

Do not replace the presigned GET with an anonymous <code>nc</code> request. A
private-object 403 proves nothing about metadata emission.

### 8.2 Form-controlled success-action redirect

Reuse the helpers from §8.1. Generate a policy that permits the redirect value,
submit to <code>post["url"]</code>, and assert the complete POST response:

~~~python
redirect = "https://example.invalid/done\r\nX-Injected: 1"
post = s3.generate_presigned_post(
    Bucket=bucket,
    Key="success-redirect",
    Fields={"success_action_redirect": redirect},
    Conditions=[["starts-with", "$success_action_redirect", ""]],
    ExpiresIn=3600,
)
post_body = multipart_body(post["fields"], b"hello")
raw = raw_request(
    post["url"],
    "POST",
    post_body,
    {"Content-Type": f"multipart/form-data; boundary={boundary}"},
)
status, fields, _ = split_response(raw)
locations = field_values(fields, "location")

assert status == 303, repr(raw)
assert len(locations) == 1, locations
assert locations[0].startswith(
    b"https://example.invalid/done  X-Injected: 1?"
), locations
assert b"\r\nX-Injected:" not in raw
~~~

The printable text <code>X-Injected: 1</code> is allowed inside the single
sanitized Location value. Only a separately framed field is a failure.

### 8.3 Pre-existing poisoned metadata and website redirects

For metadata:

1. Store an object through a pre-fix RGW, or inject the head-object attribute
   <code>user.rgw.x-amz-meta-foo</code> with raw P1.
2. Start the fixed RGW.
3. Use a presigned GET as in §8.1.
4. Require GET 200, the expected body, exactly one metadata field, and no
   <code>\r\nX-Injected:</code>. The egress-only value should contain two SP
   bytes where CR/LF were stored.

For the website <code>Location</code> path:

1. Create a website-enabled bucket and an explicitly public-readable object.
2. Using a pre-fix RGW or controlled attribute injection, store raw P1 in
   <code>user.rgw.x-amz-website-redirect-location</code>.
3. GET the object through the fixed RGW website endpoint with
   <code>raw_request()</code>.
4. Require status 301, exactly one <code>Location</code>, and no
   <code>\r\nX-Injected:</code>. The printable token may remain inside that
   one sanitized Location value.

The website request is anonymous only because the fixture is deliberately
public. Assert the 301 before inspecting Location so an authorization error
cannot false-pass.

## 9. Entry criteria

- Both target branches build the modified MIME and RGW translation units.
- <code>unittest_mime</code> passes.
- RGW starts with S3, S3 website, and Swift APIs enabled.
- Credentials, raw-socket access, and a controlled poisoned-object fixture are
  available.

## 10. Exit criteria

- Every P0 and P1 case passes.
- POST and GET recipes assert their intended success codes, required fields,
  and complete bodies before checking framing.
- No independent injected header or second response appears for P1–P11.
- P12/P13 remain unchanged; P14 is encoded as <code>=09</code> at POST/PUT
  ingress and remains literal only in SEC-23.
- SEC-12 and SEC-13 cover both redirect-emission paths.
- Standard RGW suites have no new failures versus baseline.
- Any discovered defect is triaged, with no open Sev-1/Sev-2 defect in scope.

## 11. Risks, assumptions, and notes

- A proxy should still validate and reframe upstream response headers as
  defense in depth, but the RGW fix must stand on its own.
- SDKs and curl may normalize parsed headers, so they do not replace the
  complete raw response checks.
- A safe encoded or sanitized value may contain the printable string
  <code>X-Injected</code>. Reject only an independently framed field.
- Dirty POST metadata is intentionally returned as an RFC 2047 encoded-word,
  matching PUT/COPY behavior.
- Exercise <code>rgw_extended_http_attrs</code> and custom mappings if enabled.

## 12. Traceability matrix

| Requirement / change | Test cases |
|----------------------|------------|
| Length-aware POST metadata encoding | SEC-01..06, REG-02, UNIT-01..03 |
| Strict field-name validation/omission | SEC-07, SEC-24, UNIT-04 |
| Website redirect metadata | SEC-10, SEC-11 |
| Form-controlled success-action redirect | SEC-12, §8.2 |
| Pre-existing poisoned website Location | SEC-13, §8.3 |
| Egress field-value sanitization | SEC-20..23, UNIT-05..06 |
| No functional/performance regression | REG-01..07, PERF-01, INT-01..02 |
