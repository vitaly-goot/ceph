#!/bin/bash

# USAGE ./gdb_attach.sh $OSD_PID $OSD_ID $LOG_FILE

OSD_PID=$1
OSD_ID=$2
LOG_FILE=$3

cat << EOF > /tmp/gdb.cmd
file /usr/bin/ceph-osd
set logging off
set pagination off
set logging file ${LOG_FILE}
set logging overwrite on
set logging on
print "Logging backtrace to ${LOG_FILE} for OSD ${OSD_ID}"
thread apply all bt
info threads
quit
EOF

gdb -p $OSD_PID --batch-silent -x /tmp/gdb.cmd 2>&1
rm /tmp/gdb.cmd
