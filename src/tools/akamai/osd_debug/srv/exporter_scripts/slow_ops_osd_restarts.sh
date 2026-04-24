#!/bin/bash

METRIC_NAME="slow_ops_osd_restarts"
METRIC_FILE="/var/lib/prometheus/node-exporter/slow_ops_osd_restarts.prom"
# Increment the restart count
if [ -f "$METRIC_FILE" ]; then
    RESTART_COUNT=$(grep "^$METRIC_NAME" "$METRIC_FILE" | awk '{print $2}')
    RESTART_COUNT=$((RESTART_COUNT + 1))
else
    RESTART_COUNT=1
fi

tmpfile=$(mktemp)
# Write the metric to the textfile collector directory
cat <<EOF > "$tmpfile"
# HELP $METRIC_NAME Total number of OSD restarts under slow_ops condition.
# TYPE $METRIC_NAME counter
$METRIC_NAME $RESTART_COUNT
EOF

mv -f "$tmpfile" $METRIC_FILE
chmod 644 $METRIC_FILE
chown -R prometheus:prometheus $METRIC_FILE

exit 0
