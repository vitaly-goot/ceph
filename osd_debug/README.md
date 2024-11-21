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
  /srv/gdb_attach.sh    # This script is currently commented out but can be used to attach gdb and collect backtraces from all threads on an affected OSD (requires gdb to be installed).
```

### Before Each Run: Force Log Rotation on All OSD Hosts
Execute the following to ensure logs are rotated across all hosts: 
```
logrotate -f /etc/logrotate.d/ceph-common-debug
for host in $(ceph osd tree | grep host | awk '{print $4}'); do echo $host; logrotate -f /etc/logrotate.d/ceph-common-debug; done
```

###Start the Log Debugger
Do not run this script from cron, as /srv/unwindpmp writes to the TTY, which does not work well with cron over ssh. Instead, start it in a separate session (e.g., with tmux):
```
tmux new -s osd
while true; do /srv/osd-debugger.sh& sleep 60; done
```

###Script Default Configuration
The following default configurations can be modified directly in the script:

```
  debugfolder=/var/log/debug/$(date +%Y-%m-%d)  # Folder for all debug data
  process_hist_ops=1                            # Inspect hist_ops
  process_inflight_ops=1                        # Inspect inflight_ops
  slow_ops_threshold=10                         # Trigger condition: number of slow ops
  op_duration_threshold=1                       # Trigger condition: slow ops duration (seconds)
  unwind_profiler_probes=100                    # Number of profiler probes to collect (0 disables profiler)
  profiler_run_delay=300                        # Delay before profiler run
  log_elevation_duration=0                      # Duration to elevate log debug level for acting OSDs
  session_max_duration=400                      # Maximum debug session duration
  fetch_timeout=$(( $profiler_run_delay + $log_elevation_duration + 1 )) # Timeout for fetching affected OSD logs
```

