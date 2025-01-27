#!/bin/bash

# Apply patches at patchlevel -p0. Return 0 if the patch was applied
# successfully or was previously applied. Otherwise return the patch error
# code.
#
# This is a workaround for the PATCH_COMMAND option to CMake
# ExternalProject_Add, which attempts to reapply a patch to already-patched
# code. We can't rely on using `git apply` because some build tools (rpm and
# debuild) don't preserve .git directories. So we use `patch` and scan for the
# 'Reversed (or previously applied)' message.
#
# This is far from foolproof. That message can apply to partial patches, and
# we'd misdetect that as a successful patch. However, this is a lot more
# robust than just hoping raw `patch` works.

scriptname="$(basename "$0")"

for patchfile in "$@"; do
    if [[ ! -f "$patchfile" ]]; then
        echo "$scriptname: ERROR: patch file not found: $patchfile" >&2
        exit 1
    fi

    err=$(patch --forward -r- -V never <"$patchfile")

    code=$?
    if [[ $code -eq 1 ]]; then
        if echo "$err" | grep -Eq "^Reversed \(or previously"; then
            echo "$scriptname: NOTE: patch '$patchfile' already applied, ignoring" >&2
            continue
        fi
    fi
    if [[ $code != 0 ]]; then
        echo "$scriptname: PATCH ERROR ($patchfile): $err" >&2
        exit $code
    fi
    echo "$scriptname: applied patch '$patchfile'"
done
exit 0
