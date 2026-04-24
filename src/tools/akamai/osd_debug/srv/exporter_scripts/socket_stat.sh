#!/bin/bash

METRIC_NAME="socket_stat_ephemeral_port_usage"
METRIC_FILE="/var/lib/prometheus/node-exporter/socket_stat.prom"

EPHEMERAL_PORT_USAGE=$(python3 /srv/process_ss_output.py | jq .ephemeral_ports)

tmpfile=$(mktemp)
# Write the metric to the textfile collector directory
cat <<EOF > "$tmpfile"
# HELP $METRIC_NAME node ephemeral port usage
# TYPE $METRIC_NAME gauge
$METRIC_NAME $EPHEMERAL_PORT_USAGE
EOF

mv -f "$tmpfile" $METRIC_FILE
chmod 644 $METRIC_FILE
chown -R prometheus:prometheus $METRIC_FILE

exit 0
