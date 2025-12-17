#!/usr/bin/env bash
set -euo pipefail

# A lightweight wrapper around radosgw-admin bucket stats that aggregates bucket-level object counts and size information across one or more buckets. 
# It is tailored for Ceph RGW deployments where operators need delta-aware monitoring without wiring a full metrics pipeline. Each invocation collects current totals, 
# computes per-second change rates relative to the previous sample, and appends a CSV line to a log file for later analysis.

# Usage example: 
# Continuous monitoring of a numbered bucket set inside tmux session 
# tmux new -d -s syncmon "bash -lc 'export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; LOG_FILE=/var/log/monitor_bucket_stats.csv CONCURRENCY=32 ~/monitor_stats.sh --prefix vgoot-a- --range 1-64 --loop 10 2>&1 | tee -a /var/log/monitor_bucket_stats.err'"


# ------------ CLI ------------
BUCKETS=()
PREFIX=""
RANGE=""
LOOP_INTERVAL=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--bucket)
      BUCKETS+=("$2")
      shift 2
      ;;
    -p|--prefix)
      PREFIX="$2"
      shift 2
      ;;
    -r|--range)
      RANGE="$2"     # e.g. "1-4"
      shift 2
      ;;
    --loop)
      LOOP_INTERVAL="$2"   # seconds
      shift 2
      ;;
    *)
      echo "Unknown argument: $1" >&2
      echo "Usage: $0 [--bucket NAME ...] [--prefix PREFIX --range START-END] [--loop SECONDS]" >&2
      exit 1
      ;;
  esac
done

# Build range buckets if requested
if [[ -n "$RANGE" && -n "$PREFIX" ]]; then
  start=${RANGE%%-*}
  end=${RANGE##*-}
  for i in $(seq "$start" "$end"); do
    BUCKETS+=("${PREFIX}${i}")
  done
fi

if [[ ${#BUCKETS[@]} -eq 0 ]]; then
  echo "No buckets specified. Use --bucket or --prefix/--range." >&2
  exit 1
fi

# ------------ Config ------------
CONCURRENCY=${CONCURRENCY:-8}

LOG_FILE=${LOG_FILE:-/var/log/rgw-sync-monitor.csv}
STATE_FILE=${STATE_FILE:-/var/tmp/rgw-sync-monitor.prev}

RGW_ADMIN=${RGW_ADMIN:-$(command -v radosgw-admin || echo /usr/bin/radosgw-admin)}

sudo mkdir -p "$(dirname "$LOG_FILE")" 2>/dev/null || true
sudo mkdir -p "$(dirname "$STATE_FILE")" 2>/dev/null || true
touch "$LOG_FILE"

# Header: now in KB and per *second*
if [ ! -s "$LOG_FILE" ]; then
  echo "timestamp,total_objects,total_kb,rate_obj_per_sec,rate_kb_per_sec" >> "$LOG_FILE"
fi

# ------------ Helpers ------------

# Parse `radosgw-admin bucket stats --bucket <b>` text for num_objects and size_kb_actual
parse_bucket_stats() {
  local b="$1"

  # run once, capture exit code
  local out
  if ! out=$("$RGW_ADMIN" bucket stats --bucket "$b" 2>/dev/null); then
    # radosgw-admin failed (e.g. error 2002) → return 0 0
    echo "0 0"
    return 1
  fi

  # parse JSON with jq; // 0 ensures we don’t explode on missing fields
  echo "$out" | jq -r '
    .usage["rgw.main"] as $m
    | "\(($m.num_objects // 0)) \(($m.size_kb_actual // 0))"
  '
}

sample_once() {
  local outdir; outdir=$(mktemp -d /var/tmp/rgwmon.XXXXXX)
  local total_obj=0 total_kb=0

  for b in "${BUCKETS[@]}"; do
    (
      read -r obj kb < <(parse_bucket_stats "$b")
      echo "$obj $kb" > "$outdir/$b"
    ) &

    # throttle to CONCURRENCY
    while [ "$(jobs -pr | wc -l)" -ge "$CONCURRENCY" ]; do sleep 0.05; done
  done
  wait

  # Sum results
  for b in "${BUCKETS[@]}"; do
    if [ -s "$outdir/$b" ]; then
      read -r obj kb < "$outdir/$b"
      total_obj=$(( total_obj + obj ))
      total_kb=$(( total_kb + kb ))
    fi
  done
  rm -rf "$outdir"

  # Load previous totals to compute rates (per second)
  local prev_obj=0 prev_kb=0 prev_ts=0 now rate_obj=0 rate_kb=0
  now=$(date +%s)
  if [ -f "$STATE_FILE" ]; then
    read -r prev_ts prev_obj prev_kb < "$STATE_FILE" || true
    prev_ts=${prev_ts//[^0-9]/}
    prev_obj=${prev_obj//[^0-9-]/}   # allow minus sign just in case
    prev_kb=${prev_kb//[^0-9-]/}
  fi

  if [ "${prev_ts:-0}" -gt 0 ]; then
    local dt=$(( now - prev_ts ))
    if [ "$dt" -gt 0 ]; then
      local d_obj=$(( total_obj - prev_obj ))
      local d_kb=$(( total_kb - prev_kb ))
      # per second; can be negative for net deletes
      rate_obj=$(( d_obj / dt ))
      rate_kb=$(( d_kb / dt ))
    fi
  fi

  # Store current state (for next delta)
  echo "$now $total_obj $total_kb" > "$STATE_FILE"

  local ts_iso; ts_iso=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
  echo "$ts_iso,$total_obj,$total_kb,$rate_obj,$rate_kb" >> "$LOG_FILE"
}

# ------------ Main ------------
if [[ -n "$LOOP_INTERVAL" ]]; then
  while true; do
    sample_once
    sleep "$LOOP_INTERVAL"
  done
else
  sample_once
fi

