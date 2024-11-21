#!/bin/bash 

# Usage: ./script.sh <osd_id> <debug_level> <duration_in_seconds> <dest_for_elevated_logs>

OSD_ID=$1
DEBUG_LEVEL=$2
DURATION=${3:-1800} # Default to 30 minutes if duration is not specified
DEST=${4:-/var/log/ceph/elevated}
LOCK_FILE="/tmp/osd_${OSD_ID}_debug.lock"
DEFAULT_DEBUG_LEVEL="1/5" 
LOG_FILE="/root/elevate_osd_debug.log"

exec >> "$LOG_FILE" 2>&1

osdpid=$(ps -ef  | grep '/usr/bin/ceph-osd' | grep "id $OSD_ID" | awk '{print $2}')
if [ -z "$osdpid" ]; then
    echo "[$(date)] OSD process with ID $OSD_ID does not exist. Exiting."
    exit 1
fi

cleanup() {
    echo "[$(date)] Script interrupted. Cleaning up and resetting debug level for OSD $OSD_ID"
    #ceph tell osd.$OSD_ID config set debug_osd $DEFAULT_DEBUG_LEVEL
    ceph --admin-daemon /var/run/ceph/ceph-osd.$OSD_ID.asok config set debug_osd $DEFAULT_DEBUG_LEVEL
    if [ -f ${OSD_LOG}.ORG ]; then
        mv ${OSD_LOG}.ORG $OSD_LOG
        kill -1 $osdpid
    fi	
    rsync -a ${OSD_LOG}.ELEVATED $DEST
    echo "[$(date)] Cleanup completed."
    rm -f "$LOCK_FILE"
    exit 2
}

trap cleanup SIGINT SIGTERM

if [ -z "$OSD_ID" ] || [ -z "$DEBUG_LEVEL" ]; then
    echo "Usage: $0 <osd_id> <debug_level> [duration_in_seconds] [dest_for_elevated_logs]"
    exit 1
fi

if [ -f "$LOCK_FILE" ]; then
    LOCK_PID=$(cat "$LOCK_FILE")
    if ps -p "$LOCK_PID" > /dev/null 2>&1; then
        echo "[$(date)] Another instance of the script is already running for OSD $OSD_ID. Exiting."
        exit 1
    else
        echo "[$(date)] Stale lock file detected. Removing."
        rm -f "$LOCK_FILE"
    fi
fi

echo $$ > "$LOCK_FILE"

if [ "$DEST" = "/var/log/ceph/elevated" ] && [ ! -d "$DEST" ]; then
    mkdir -p "$DEST" || { echo "Failed to create directory $DEST"; exit 1; }
fi
OSD_LOG=/var/log/ceph/ceph-osd.${OSD_ID}.log
mv $OSD_LOG ${OSD_LOG}.ORG
kill -1 $osdpid
echo "[$(date)] Setting debug level for OSD $OSD_ID to $DEBUG_LEVEL for $DURATION seconds"
#ceph tell osd.$OSD_ID config set debug_osd $DEBUG_LEVEL
ceph --admin-daemon /var/run/ceph/ceph-osd.$OSD_ID.asok config set debug_osd $DEBUG_LEVEL
sleep 3
mv $OSD_LOG ${OSD_LOG}.ELEVATED
sleep "$DURATION"

# Reset the debug level to default
echo "[$(date)] Resetting debug level for OSD $OSD_ID to default ($DEFAULT_DEBUG_LEVEL)"
#ceph tell osd.$OSD_ID config set debug_osd $DEFAULT_DEBUG_LEVEL
ceph --admin-daemon /var/run/ceph/ceph-osd.$OSD_ID.asok config set debug_osd $DEFAULT_DEBUG_LEVEL

mv ${OSD_LOG}.ORG $OSD_LOG
kill -1 $osdpid
if [ "$DEST" = "/var/log/ceph/elevated" ] && [ ! -d "$DEST" ]; then
    mkdir -p $DEST
fi    
gzip ${OSD_LOG}.ELEVATED
if [ -d "$DEST" ]; then
  rsync -a --remove-source-files ${OSD_LOG}.ELEVATED.gz $DEST/
else
  rsync -a --remove-source-files ${OSD_LOG}.ELEVATED.gz $DEST.gz
fi	
echo "[$(date)] Done."
rm -f "$LOCK_FILE"
exit 0
