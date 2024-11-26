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
  #echo "Aborting"
  exit 1
fi

cur_time=$(date +%s)
timestamp=$(date +%Y-%m-%d-%H-%M-%S)
if [[ $DEBUG -eq 1 ]]; then
    debugfolder=/var/log/debug/$(date +%Y-%m-%d)
    slowosds="osd.125"
    process_hist_ops=1
    process_inflight_ops=0
    slow_ops_size_threshold=0
    slow_ops_duration_threshold=0
    unwind_profiler_probes=100
    profiler_run_delay=300
    collect_perf_record=1
    attach_gdb=1
    restart_osd=0
    log_elevation_duration=0
    session_max_duration=86400
    session_end_enforced=1
else
    debugfolder=/var/log/debug/$(date +%Y-%m-%d)
    process_hist_ops=0
    process_inflight_ops=0
    slow_ops_size_threshold=10
    slow_ops_duration_threshold=1
    unwind_profiler_probes=0
    profiler_run_delay=0
    collect_perf_record=0
    attach_gdb=0
    restart_osd=1
    log_elevation_duration=0
    session_max_duration=86400
    session_end_enforced=0
fi	

push_unique_to_logs_tobe_fetched() {
    local key=$1
    local value=$2

    # Retrieve existing list as a space-separated string
    local existing_array="${logs_tobe_fetched[$key]}"

    # Check if the value already exists in the list
    if [[ " $existing_array " =~ " $value " ]]; then
        echo "Value '$value' already exists for key '$key'. Skipping."
        return
    fi

    # Append the new value (space-separated)
    if [[ -z "$existing_array" ]]; then
        logs_tobe_fetched[$key]="$value"
    else
        logs_tobe_fetched[$key]="${existing_array} $value"
    fi
}

dump_logs_tobe_fetched_as_shell_script() {
    local filename=$1
    > "$filename"  # Clear the file
    echo "declare -g -A logs_tobe_fetched=()" > "$filename"
    for key in "${!logs_tobe_fetched[@]}"; do
        echo "logs_tobe_fetched[\"$key\"]=\"${logs_tobe_fetched[$key]}\"" >> "$filename"
    done
}

fetch_logs() {
    log_message $start_time "fetch_logs"
    for host in "${!logs_tobe_fetched[@]}"; do
        echo "$host:"
        rsync -a $host:${debugfolder}/ ${debugfolder} 

        # Extract the space-separated string into an array
        IFS=' ' read -r -a osd_list <<< "${logs_tobe_fetched[$host]}"

        echo fetch_osd $osd_list
        for osdid in "${osd_list[@]}"; do
            ssh $host "gzip -c /var/log/ceph/ceph-osd.${osdid}.log" > ${debugfolder}/${session_start}-${host}-osd.${osdid}.log.gz
            log_message $start_time "$host gzip -c /var/log/ceph/ceph-osd.${osdid}.log > ${debugfolder}/${session_start}-${host}-osd.${osdid}.log.gz"
        done
    done
}

function test_fetch_logs() {
    echo "declare -g -A logs_tobe_fetched=()" >> /tmp/log_fetch_test
    source /tmp/log_fetch_test
    push_unique_to_logs_tobe_fetched "bs3133-lax3" "567" 
    dump_logs_tobe_fetched_as_shell_script /tmp/log_fetch_test
    source /tmp/log_fetch_test
    push_unique_to_logs_tobe_fetched "bs3135-lax3" "621" 
    push_unique_to_logs_tobe_fetched "bs3139-lax3" "815" 
    dump_logs_tobe_fetched_as_shell_script /tmp/log_fetch_test
    source /tmp/log_fetch_test
    push_unique_to_logs_tobe_fetched "bs3133-lax3" "567" 
    push_unique_to_logs_tobe_fetched "bs3139-lax3" "815" 
    dump_logs_tobe_fetched_as_shell_script /tmp/log_fetch_test
    source /tmp/log_fetch_test
    fetch_logs
}

report_time_wait() {
    for host in $(ceph osd tree | grep host | awk '{print $4}'); do 
        log_message $start_time "Sockets stats $host $(ss -ant | awk '{print $1}' | sort | uniq -c | grep -v State | awk '{printf "%s: %s, ", $2, $1}' | sed 's/, $//')"
    done
}

source /srv/osd-debugger.rc 

if [[ ! -v session_start ]]; then
    session_start=$start_time
    echo "declare -g -A logs_tobe_fetched=()" > /srv/osd-debugger_session_$session_start.rc
    echo "============================================" >> $LOG_FILE
    echo "$timestamp New Session started" >> $LOG_FILE  
fi


source /srv/osd-debugger_session_$session_start.rc

if [[ -v session_halt ]]; then
    exit 2
fi

echo "$timestamp Session iteration started" >> $LOG_FILE

if [[ ! -v profiler_schedule ]]; then
    profiler_schedule=$(( $session_start + $profiler_run_delay ))
fi    

if [[ ! -v session_end_schedule ]]; then
    session_end_schedule=$(( $session_start + $session_max_duration ))
fi    

if (( $cur_time > $session_end_schedule || $session_end_enforced )); then
    echo "Session limit reached. To start new session remove /srv/osd-debugger.rc" >> $LOG_FILE
    fetch_logs
    cp $LOG_FILE  ${debugfolder}/osd-debugger-session-${session_start}-timeline
    echo "export session_halt=1" >> /srv/osd-debugger.rc
    exit 0
fi

if (( $cur_time < $profiler_schedule )); then
    unwind_profiler_probes=0 # disable profiler
else 
    profiler_schedule=$(( $start_time + profiler_run_delay ))
fi

# Saving variables to osd-debugger.rc
cat <<EOF > /srv/osd-debugger.rc
export session_start=$session_start
export profiler_schedule=$profiler_schedule
export session_end_schedule=$session_end_schedule
EOF

log_message $start_time "session elaped time $(( $cur_time - $session_start )), session ends in $(( $session_end_schedule -  $cur_time ))"

report_time_wait

slowosds=$(ceph health detail -f json | jq -r 2>/dev/null '.checks.SLOW_OPS.summary.message | capture("(?<osds>osd\\.([0-9]+)(,osd\\.[0-9]+)*)").osds | split(",")[]') || slowosds=""
if [ "$slowosds" = "" ]
then
  #for host in $(ceph osd tree | grep host | awk '{print $4}'); do rsync -a $host:${debugfolder} /var/log/debug/; done
  log_message $start_time "all clear"
  exit 0
fi

freespace=$(df $debugfolder|tail -n1 | awk '{print $4}')
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

process_remote_osd() {
    local osdid=$1	
    local osdhost=$(ceph osd find $osdid| jq '.host'|cut -d . -f 1|cut -f 2 -d '"')
    local osdpid=$(ssh root@${osdhost} "ps -ef | grep 'id $osdid' | grep -v grep | awk '{print \$2}'")
    local log_prefix=${debugfolder}/${timestamp}-${osdhost}-osd.${osdid}
    log_message $start_time "Processing OSD: $osdid on host $osdhost $log_prefix"
    push_unique_to_logs_tobe_fetched "$osdhost" "$osdid" 


    ssh root@${osdhost} "
        [ -d $debugfolder ] || mkdir -p $debugfolder
        [ ! -f ${log_prefix}-historic_ops ] && ceph daemon osd.$osdid dump_historic_ops > ${log_prefix}-historic_ops 2>&1 
        [ ! -f ${log_prefix}-inflight ] && ceph daemon osd.$osdid dump_ops_in_flight > ${log_prefix}-inflight  2>&1 
    " 
    log_message $start_time "Histograms of historic_ops and inflight were dumped."
    ssh root@${osdhost} "iostat -x 1 10" > ${log_prefix}-iostat 2>&1
    log_message $start_time "iostat -x 1 10 in ${log_prefix}-iostat"
    log_message $start_time "Offended OSD Sockets stats $(python3 process_ss_output.py)"
    if [[ $unwind_profiler_probes -gt 0 ]]; then 
        log_message $start_time "starting unwind profiler"
        ssh -t root@${osdhost} "/srv/unwind_attach.sh $osdpid $osdid ${log_prefix}-unwindpmp $unwind_profiler_probes"
        log_message $start_time "/srv/unwind_attach.sh $osdpid $osdid ${log_prefix}-unwindpmp $unwind_profiler_probes"
    fi    
    if [[ $collect_perf_record -eq 1 ]]; then
        log_message $start_time "starting perf record"
        #ssh root@${osdhost} "perf record -p $osdpid -F 20 --call-graph dwarf -o ${log_prefix}-perf-data-dwarf  -- sleep 10" > /dev/null 2>&1 
        ssh root@${osdhost} "perf record -p $osdpid -F 20 -g -o ${log_prefix}-perf-data  -- sleep 10"
        log_message $start_time "perf record -p $osdpid"
    fi
    if [[ $attach_gdb -eq 1 ]]; then
        log_message $start_time "attaching gdb"
        ssh root@${osdhost} "/srv/gdb_attach.sh $osdpid $osdid ${log_prefix}-gdb"
        log_message $start_time "/srv/gdb_attach.sh $osdpid $osdid ${log_prefix}-gdb"
    fi
    if [[ $restart_osd -eq 1 ]]; then
        log_message $start_time "Restarting OSD: $osdid on host $osdhost $log_prefix"
        ssh root@${osdhost} "systemctl restart ceph-osd@$osdid"
        echo "==== post restart ===="  >> ${log_prefix}-iostat
        ssh root@${osdhost} "iostat -x 1 10" >> ${log_prefix}-iostat 2>&1
        log_message $start_time "iostat -x 1 10 after restart appended  to ${log_prefix}-iostat"
        log_message $start_time "Offended OSD Sockets stats after restart $(python3 process_ss_output.py)"
    fi    
    log_message $start_time "processed OSD: $osdid on host $osdhost, sockets in time-wait $(ssh $host ss -ant state time-wait | wc -l)"

    #    perf record -p $osdpid -F 20 --call-graph dwarf -o ${log_prefix}-perf-data-dwarf  -- sleep 10 > /dev/null 2>&1 
    #ceph tell $osd perf dump > ${log_prefix}-perfdump & 
    #log_message $start_time "perf dump"
}

slow_ops_inspect() {
    local inspect=$1
    local opsnumber=$2
    local run_process=$3
    local msg=$4
    if [ $opsnumber -gt $slow_ops_size_threshold ]; then
        pglist=$(jq --argjson duration "$slow_ops_duration_threshold"  -r '.ops[] | select(.description | contains("osd_op")) | select(.duration > $duration) | (.description | capture("(?<pg>[0-9]+\\.[0-9a-z]+s[0-9])").pg | sub("s[0-9]$"; ""))' $inspect | sort | uniq)
        pgs=$(echo "$pglist" | tr '\n' ' ')
        log_message $start_time "$msg slow operation inspect $inspect, threshold $slow_ops_duration_threshold seconds, affected PGs ( $pgs)"
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
                    if [[ $restart_osd -eq 1 ]]; then
                        return 0
                    fi
                done
            else     
                log_message $start_time "skipping remote OSD processing..."
            fi
        done
    fi
}

[ -d $debugfolder ] || mkdir -p $debugfolder
for osd in $slowosds
do
    osdhost=`ceph osd find $osd| jq '.host'|cut -d . -f 1|cut -f 2 -d '"'`
    osdid=$(echo $osd|cut -f 2 -d .)
    log_prefix=${debugfolder}/${timestamp}-${osdhost}-osd.${osdid}
    log_message $start_time "ssh root@${osdhost} ceph daemon $osd dump_historic_ops"
    ssh root@${osdhost} "ceph daemon $osd dump_historic_ops" > ${log_prefix}-historic_ops
    log_message $start_time "ssh root@${osdhost} ceph daemon $osd dump_ops_in_flight"
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

dump_logs_tobe_fetched_as_shell_script /srv/osd-debugger_session_$session_start.rc

log_message $start_time "all set"
exit 0
