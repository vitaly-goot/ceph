#!/bin/bash

DEBUG=0
if [[ "$1" == "-d" ]]; then
    DEBUG=1
    shift 
fi

start_time=$(date +%s)
LOG_FILE=/root/osd-debugger.log

log_message() {
  local start_time=$1
  local message=$2
  local end_time=$(date +%s)
  local elapsed=$(( end_time - start_time ))
  echo "+ [$elapsed] $message"  >> $LOG_FILE
}

count=$(ps -ef|grep $0|grep -v grep|grep -c bash)
#echo -n "$count at $(date) " >> $LOG_FILE
if [ "$count" -gt 2 ]
then
  #echo "Aborting" >> $LOG_FILE
  exit 1
fi
echo "$start_time Iteration started" >> $LOG_FILE

if [[ $DEBUG -eq 1 ]]; then
    debugfolder=/var/log/debug/$(date +%Y-%m-%d)
    slowosds="osd.125"
    process_hist_ops=1
    process_inflight_ops=0
    slow_ops_threshold=0
    op_duration_threshold=0
    unwind_profiler_probes=100
    profiler_run_delay=300
    log_elevation_duration=0
    session_max_duration=86400
    fetch_timeout=0
else
    debugfolder=/var/log/debug/$(date +%Y-%m-%d)
    process_hist_ops=1
    process_inflight_ops=0
    slow_ops_threshold=10
    op_duration_threshold=1
    unwind_profiler_probes=100
    profiler_run_delay=30
    log_elevation_duration=0
    session_max_duration=400
    fetch_timeout=$(( $profiler_run_delay + $log_elevation_duration + 1 ))
fi	

source /srv/osd-debugger.rc 

if [[ ! -v session_start ]]; then
    session_start=$start_time
    echo "============================================" >> $LOG_FILE
    echo "$session_start session started" >> $LOG_FILE  
fi

if [[ ! -v profiler_schedule ]]; then
    profiler_schedule=$(( $session_start + $profiler_run_delay ))
fi    

if [[ ! -v fetch_schedule ]]; then
    fetch_schedule=$(( $session_start + $fetch_timeout ))
fi    

if [[ ! -v session_end_schedule ]]; then
    session_end_schedule=$(( $session_start + $session_max_duration ))
fi    

cur_time=$(date +%s)
if [[ $cur_time > $session_end_schedule ]]; then
  echo "Session limit reached. To start new session remove /srv/osd-debugger.rc" >> $LOG_FILE
  exit 2
fi


if [[ $cur_time < $profiler_schedule ]]; then
    unwind_profiler_probes=0 # disable profiler
else 
    profiler_schedule=$(( $start_time + profiler_run_delay ))
fi

if [[ fetch_timeout -eq 0 || $cur_time < $fetch_schedule ]]; then
    do_log_fetch=0 # disable OSD log fetching
else 
    fetch_schedule=$(( $start_time + fetch_timeout ))
    do_log_fetch=1 
fi

log_message $start_time "session elaped time $(( $cur_time - $session_start )), session ends in $(( $session_end_schedule -  $cur_time )), fetch log $do_log_fetch, run profiler $unwind_profiler_probes"


# Saving variables to osd-debugger.rc
cat <<EOF > /srv/osd-debugger.rc
export session_start=$session_start
export profiler_schedule=$profiler_schedule
export fetch_schedule=$fetch_schedule
export session_end_schedule=$session_end_schedule
EOF

sleep 5
exit 0

slowosds=$(ceph health detail -f json | jq -r 2>/dev/null '.checks.SLOW_OPS.summary.message | capture("(?<osds>osd\\.([0-9]+)(,osd\\.[0-9]+)*)").osds | split(",")[]') || slowosds=""
if [ "$slowosds" = "" ]
then
  log_message $start_time "all clear"
  exit 0
fi

freespace=$(df $debugfolder|tail -n1 | awk '{print $4}')
timestamp=$(date +%Y-%m-%d-%H-%M-%S)
#freelimit=52428800 #50G in 1k blocks
#if [ $freespace -gt $freelimit ]
#then
#  timeout 62 tcpdump -n -i any -s 96 > ${debugfolder}/${timestamp}-$(hostname -s).pcap &
#fi
ceph health detail | sed "s/^/$timestamp /" >> ${debugfolder}/ceph-status


# vgoot
# reset perf stats (for later perf dump)
# after reset we need to wait for a few minutes to get performance data accumulate enough operational metrics for meaningful insight.
#for osd in $slowosds
#do
#  osdhost=`ceph osd find $osd| jq '.host'|cut -d . -f 1|cut -f 2 -d '"'`
#  ssh root@${osdhost} "ceph daemon $osd perf reset all"
#done
#sleep 10

elevate_osd_log() {
    local osdid=$1	
    local osdhost=$(ceph osd find $osdid| jq '.host'|cut -d . -f 1|cut -f 2 -d '"')
    local osdpid=$(ssh root@${osdhost} "ps -ef | grep 'id $osdid' | grep -v grep | awk '{print \$2}'")
    local log_prefix=${debugfolder}/${timestamp}-${osdhost}-osd.${osdid}
    local level=20
    log_message $start_time "Elevate OSD $osdid log level:  $level"
    ssh root@${osdhost} "
        [ -d $debugfolder ] || mkdir -p $debugfolder
        nohup /srv/elevate_osd_log.sh $osdid $level $log_elevation_duration $log_prefix-osdlog-elevated > /dev/null 2>&1 &
    "    
}

declare -A hosts

process_remote_osd() {
    local osdid=$1	
    local osdhost=$(ceph osd find $osdid| jq '.host'|cut -d . -f 1|cut -f 2 -d '"')
    local osdpid=$(ssh root@${osdhost} "ps -ef | grep 'id $osdid' | grep -v grep | awk '{print \$2}'")
    local log_prefix=${debugfolder}/${timestamp}-${osdhost}-osd.${osdid}
    log_message $start_time "Processing OSD: $osdid on host $osdhost $log_prefix"
    hosts["$osdhost"]="$osdid"

    ssh root@${osdhost} "
        [ -d $debugfolder ] || mkdir -p $debugfolder
        [ ! -f ${log_prefix}-historic_ops ] && ceph daemon osd.$osdid dump_historic_ops > ${log_prefix}-historic_ops 2>&1 
        [ ! -f ${log_prefix}-inflight ] && ceph daemon osd.$osdid dump_ops_in_flight > ${log_prefix}-inflight  2>&1 
    " 
    if [[ $unwind_profiler_probes -gt 0 ]]; then 
        log_message $start_time "starting profiler"
        ssh -t root@${osdhost} "/srv/unwind_attach.sh $osdpid $osdid ${log_prefix}-unwindpmp $unwind_profiler_probes"
        log_message $start_time "/srv/unwind_attach.sh $osdpid $osdid ${log_prefix}-unwindpmp $unwind_profiler_probes"
    fi    

    #    perf record -p $osdpid -F 20 --call-graph dwarf -o ${log_prefix}-perf-data-dwarf  -- sleep 10 > /dev/null 2>&1 
    #ceph tell $osd perf dump > ${log_prefix}-perfdump & 
    #log_message $start_time "perf dump"
    #ssh root@${osdhost} "/srv/gdb_attach.sh $osdpid $osdid ${log_prefix}-gdb"
    #log_message $start_time "gdb_attach.sh"
    #ssh root@${osdhost} "perf record -p $osdpid -F 20 -g -o $debugfolder/perf-data-${timestamp}-$osd  -- sleep 10"&
    #log_message $start_time "perf record"
}

slow_ops_inspect() {
    local inspect=$1
    local opsnumber=$2
    local run_process=$3
    local msg=$4
    if [ $opsnumber -gt $slow_ops_threshold ]; then
        pglist=$(jq --argjson duration "$op_duration_threshold"  -r '.ops[] | select(.description | contains("osd_op")) | select(.duration > $duration) | (.description | capture("(?<pg>[0-9]+\\.[0-9a-z]+s[0-9])").pg | sub("s[0-9]$"; ""))' $inspect | sort | uniq)
        pgs=$(echo "$pglist" | tr '\n' ' ')
        log_message $start_time "$msg slow operation inspect $inspect, threshold $op_duration_threshold seconds, affected PGs ( $pgs)"
        for pg in $pglist; do
            acting_osds=$(ceph pg map $pg | grep 'acting' | awk -F'[][]' '{print $2}')
            log_message $start_time "pg $pg acting_osds $acting_osds"
            if [[ $run_process -eq 1 ]]; then
                if [[ $log_elevation_duration -gt 0 ]]; then
                    for acting_osd in $(echo $acting_osds | tr ',' ' '); do
                        elevate_osd_log $acting_osd
                    done
                fi
                for acting_osd in $(echo $acting_osds | tr ',' ' '); do
                    process_remote_osd $acting_osd
                done
            else     
                log_message $start_time "skipping remote OSD processing..."
            fi
        done
    fi
}

for osd in $slowosds
do
    osdhost=`ceph osd find $osd| jq '.host'|cut -d . -f 1|cut -f 2 -d '"'`
    osdid=$(echo $osd|cut -f 2 -d .)
    log_prefix=${debugfolder}/${timestamp}-${osdhost}-osd.${osdid}
    ssh root@${osdhost} "ceph daemon $osd dump_historic_ops" > ${log_prefix}-historic_ops
    ssh root@${osdhost} "ceph daemon $osd dump_ops_in_flight" > ${log_prefix}-inflight
    #opsnumber=$(grep -c osd_op ${log_prefix}-inflight)
    n_hist=$(grep -c osd_op ${log_prefix}-historic_ops)
    n_inflight=$(grep -c osd_op ${log_prefix}-inflight)
    log_message $start_time "OSD operation snapshot $osd hist($n_hist) inflight($n_inflight) reported to $log_prefix"
    hist_duration=$(jq '.ops | map(select(.description | contains("osd_op")) | .duration) | {max: max, average: (add / length), count: length}' ${log_prefix}-historic_ops | tr -d '\n')
    inflight_duration=$(jq '.ops | map(select(.description | contains("osd_op")) | .duration) | {max: max, average: (add / length), count: length}' ${log_prefix}-inflight | tr -d '\n')
    log_message $start_time "hist operation duration $hist_duration"
    log_message $start_time "inflight operation duration $inflight_duration"
    slow_ops_inspect ${log_prefix}-historic_ops $n_hist $process_hist_ops "hist_ops" 
    slow_ops_inspect ${log_prefix}-inflight $n_inflight $process_inflight_ops "inflight_ops"
done

if [[ $do_log_fetch -gt 0 ]]
    if [ ${#hosts[@]} -gt 0 ]; then
        log_message $start_time "do_log_fetch"
        for host in "${!hosts[@]}"; do
            if [[ $log_elevation_duration -gt 0 ]]; then
                rsync -a $host:${debugfolder} /var/log/debug/
                log_message $start_time "rsync elevated osd log from $host"
            else
                rsync -a $host:${debugfolder} /var/log/debug/
                ssh $host "gzip -c /var/log/ceph/ceph-osd.${hosts[$host]}.log" > ${debugfolder}/${timestamp}-${host}-osd.${hosts[$host]}.log.gz
                log_message $start_time "ssh $host \"gzip -c /var/log/ceph/ceph-osd.${hosts[$host]}.log\" > ${debugfolder}/${host}-osd.${hosts[$host]}.log"
            fi
        done
    fi
    cp $LOG_FILE  ${debugfolder}/${session_start}-timeline
fi

log_message $start_time "all set"
exit 0
