# OSD-Debugger Toolkit

### Compile Mark's Profiler Tool
Mark's profiler tool can be found at [https://github.com/markhpc/uwpmp](https://github.com/markhpc/uwpmp). Below is an example for compiling it on `ubuntu:focal`:

```
Run a Docker container with Ubuntu focal
docker run -it ubuntu:focal /bin/bash

Install necessary dependencies
apt update
apt install git cmake elfutils g++ autoconf automake libtool libelf-dev libdw-dev

Clone and build the profiler
git clone https://github.com/markhpc/uwpmp.git
cd uwpmp
mkdir build
cd build
cmake ..
make
```
Once compiled, install the unwindpmp executable on all OSDs under the /srv/ directory.

### Install on One MON Node
Copy and configure the following script:
/srv/osd-debugger.sh

### Install on All OSD Hosts
Ensure the following files and scripts are deployed:
```
  Optionally: all OSD binaries compiled with debug symbols.
  /srv/unwind_attach.sh
  /etc/logrotate.d/ceph-common-debug
  /srv/elevate_osd_log.sh
  /srv/gdb_attach.sh               
  /srv/process_ss_output.py  # detailed socket stat info 
```

### Before Each Run: Force Log Rotation on All OSD Hosts
Optionally execute the following to trim OSD logs before debug session start: 
```
logrotate -f /etc/logrotate.d/ceph-common-debug
for host in $(ceph osd tree | grep host | awk '{print $4}'); do echo $host; logrotate -f /etc/logrotate.d/ceph-common-debug; done
```

### Start the Log Debugger
Do not run this script from cron, as /srv/unwindpmp writes to the TTY, which does not work well with cron over ssh. Instead, start it in a separate session (e.g., with tmux):
```
tmux new -s osd
while true; do /srv/osd-debugger.sh& sleep 60; done
```

### Script Default Configuration
The following default configurations can be modified directly in the script:

```
  debugfolder=/var/log/debug/$(date +%Y-%m-%d)  # Folder for all debug data
  process_hist_ops=1                            # Inspect hist_ops
  process_inflight_ops=1                        # Inspect inflight_ops
  slow_ops_size_threshold=10                    # Trigger condition: number of slow ops
  slow_ops_duration_threshold=1                 # Trigger condition: slow ops duration (seconds)
  unwind_profiler_probes=100                    # Number of profiler probes to collect (0 disables profiler)
  profiler_run_delay=300                        # Delay before profiler run
  collect_perf_record=1                         # Capture performance data with 'perf record'
  attach_gdb=0                                  # Attach with gdb and capture a snapshot of all threads.
  log_elevation_duration=0                      # Duration to elevate log debug level for acting OSDs
  restart_osd=0                                 # Restart *primary* OSD for affected PG when slow_ops_size_threshold && slow_ops_duration_threshold confition met 
  session_max_duration=400                      # Maximum duration for a debug session; at the end, logs from affected OSDs are retrieved from remote hosts to $debugfolder.
  session_end_enforced=0                        # Forcefully terminate the debug session and fetch all logs.
```

