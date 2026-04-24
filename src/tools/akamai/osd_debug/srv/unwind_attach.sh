#!/bin/bash

# USAGE ./unwind_attach.sh $OSD_PID $OSD_ID $LOG_FILE $N

OSD_PID=$1
OSD_ID=$2
LOG_FILE=$3
N=$4

exec >> "$LOG_FILE" 2>&1

script -q -c "
echo Unwind backtrace to ${LOG_FILE} for OSD ${OSD_ID} with $N probes
/srv/unwindpmp -n $N -p $OSD_PID
"
