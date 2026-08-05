#!/usr/bin/env bash
set -ex

mydir=$(dirname "$0")

python3 -m venv "$mydir"
source "$mydir/bin/activate"
pip install --upgrade pip
pip install boto3 requests

"$mydir/bin/python3" "$mydir/test_rgw_post_object_security.py"

deactivate
echo OK.
