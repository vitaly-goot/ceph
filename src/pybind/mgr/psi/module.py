import json
import threading
import time
import concurrent.futures
import errno
from datetime import datetime, timedelta
from mgr_module import MgrModule, CLICommand

class Module(MgrModule):
    MODULE_OPTIONS = [
        {
            'name': 'idle_cell_threshold',
            'type': 'float',
            'default': 15.0,
            'desc': 'Minimum CPU idle percent required to exit throttling (0 disables check)'
        },
        {
            'name': 'idle_floor_threshold',
            'type': 'float',
            'default': 7.0,
            'desc': 'Maximum CPU idle percent to allow triggering throttling (0 disables check)'
        },
        {
            'name': 'trigger_count',
            'type': 'int',
            'default': 10,
            'desc': 'Number of hosts that must meet condition to trigger or stop throttling'
        },
        {
            'name': 'throttle_bytes',
            'type': 'str',
            'default': '10000',
            'desc': 'Value to set for bluestore_throttle_bytes when enabling throttle'
        },
        {
            'name': 'interval',
            'type': 'int',
            'default': 60,
            'min': 10,
            'max': 300,
            'desc': 'Interval (in seconds) between PSI checks'
        },
        {
            'name': 'start_avg300_threshold',
            'type': 'float',
            'default': 80.0,
            'desc': 'Threshold for avg300 to trigger config change'
        },
        {
            'name': 'stop_avg300_threshold',
            'type': 'float',
            'default': 50.0,
            'desc': 'avg300 threshold below which throttle is removed'
        },
        {
            'name': 'stop_avg60_threshold',
            'type': 'float',
            'default': 40.0,
            'desc': 'avg60 threshold below which throttle is removed'
        },
        {
            'name': 'cooldown_seconds',
            'type': 'int',
            'default': 600,
            'max': 1800,
            'desc': 'Cool-down time (in seconds) after throttle removal to avoid flipping'
        }
    ]

    def __init__(self, *args, **kwargs):
        super(Module, self).__init__(*args, **kwargs)
        self._throttle_history = json.loads(self.get_store('throttle_history') or '[]')
        self._psi_snapshot = {}
        self.run = False
        self.thread = None
        timestamp = self.get_store('last_throttle_removal')
        if timestamp:
            self.last_throttle_removal = datetime.fromisoformat(timestamp)
        else:
            self.last_throttle_removal = None                
        timestamp = self.get_store('throttle_enabled_time')
        if timestamp:
            self.throttle_enabled_time = datetime.fromisoformat(timestamp)
            self.log.info(f"Throttle protection is active on initialization, throtteling in effect since {self.get_store('throttle_enabled_time')}") 
        else:
            self.throttle_enabled_time = None

    def serve(self):
        self.run = True
        self.thread = threading.Thread(target=self._collect_loop)
        self.thread.start()

    def shutdown(self):
        self.run = False
        if self.thread:
            self.thread.join()

    def _collect_loop(self):
        while self.run:
            try:
                self.collect_cluster_psi()
                self.adjust_throttling()
            except Exception as e:
                self.log.error(f"Collection loop error: {str(e)}")
            time.sleep(int(self.get_module_option('interval')))

    def collect_cluster_psi(self):
        host_osd_map = self.get_one_active_osd_per_host()
        result = {}

        def fetch_psi(host, osd_id):
            self.log.debug(f"Fetch PSI on host {host} osd.{osd_id}")
            rc, out, err = self.osd_command({
                'prefix': 'psi', 'id': str(osd_id), 'format': 'json'
            })
            if rc == 0:
                return host, out.strip()
            else:
                self.log.warning(f"Unable to collect PSI on host '{host}' (osd.{osd_id}): {err.strip()}")
                return host, None

        with concurrent.futures.ThreadPoolExecutor(max_workers=20) as executor:
            futures = [executor.submit(fetch_psi, host, osd_id) for host, osd_id in host_osd_map.items()]
            for future in concurrent.futures.as_completed(futures):
                host, data = future.result()
                if data:
                    result[host] = json.loads(data)

        self._psi_snapshot = result

    def parse_psi(self, psi):
        if "cpu_pressure" in psi:
            cpu_pressure = psi["cpu_pressure"]
        else:
            cpu_pressure = psi.get("psi", {})

        some = cpu_pressure.get("some", {})
        avg300 = float(some.get("avg300", 0))
        avg60 = float(some.get("avg60", 0))

        idle = psi.get("cpu_idle", {})
        idle_percent = float(idle.get("percent", 100.0))

        return avg300, avg60, idle_percent

    def get_one_active_osd_per_host(self):
        active_osds = set(osd['osd'] for osd in self.get("osd_map")['osds'] if osd["up"] == 1 and osd["in"] == 1)
        metadata = self.get("osd_metadata")
        host_to_osd = {}
        for osd_id_str, meta in metadata.items():
            osd_id = int(osd_id_str)
            if osd_id in active_osds:
                host = meta.get("hostname")
                if host and host not in host_to_osd:
                    host_to_osd[host] = osd_id
        self.log.info(host_to_osd)
        return host_to_osd

    def is_throttle_enabled(self):
        return self.throttle_enabled_time != None

    def cooldown_expired(self):
        if not self.last_throttle_removal:
            return True
        cooldown_seconds = int(self.get_module_option('cooldown_seconds'))
        return datetime.utcnow() >= self.last_throttle_removal + timedelta(seconds=cooldown_seconds)

    def adjust_throttling(self):      
        exceeded_hosts = 0
        below_hosts = 0

        for host, psi_json in self._psi_snapshot.items():
            avg300, avg60, idle_percent = self.parse_psi(psi_json)
            if avg300 > self.get_module_option('start_avg300_threshold') and (self.get_module_option('idle_floor_threshold') == 0 or idle_percent < self.get_module_option('idle_floor_threshold')):
                exceeded_hosts += 1
            elif avg300 < self.get_module_option('stop_avg300_threshold') and avg60 < self.get_module_option('stop_avg60_threshold') and (self.get_module_option('idle_cell_threshold') == 0 or idle_percent > self.get_module_option('idle_cell_threshold')):
                below_hosts += 1

        self.log.info(f"{exceeded_hosts} hosts exceeded avg300 > {self.get_module_option('start_avg300_threshold')}")
        self.log.info(f"{below_hosts} hosts below stop threshold avg300 < {self.get_module_option('stop_avg300_threshold')} and avg60 < {self.get_module_option('stop_avg60_threshold')}")

        if exceeded_hosts >= self.get_module_option('trigger_count') and not self.is_throttle_enabled():
            self.throttle_on(exceeded_hosts)
        elif below_hosts >= self.get_module_option('trigger_count') and self.is_throttle_enabled() and self.cooldown_expired():
            self.throttle_off(below_hosts)

    def throttle_on(self, exceeded_hosts):
        self.throttle_enabled_time = datetime.utcnow()
        self.set_store('throttle_enabled_time', self.throttle_enabled_time.isoformat())
        self._throttle_history.append({"start": self.throttle_enabled_time.isoformat(), "end": None})
        self._throttle_history = self._throttle_history[-10:]
        self.set_store('throttle_history', json.dumps(self._throttle_history))
        self.set_health_checks({
            'PSI_THROTTLE_ACTIVE': {
                'severity': 'warning',
                'summary': f'Throttling enabled due to high CPU pressure at {self.throttle_enabled_time.isoformat()}',
                'detail': [f'Treashold exceeded on {exceeded_hosts} hosts']
            }
        })
        # enable throttling 
        self.mon_command({
                'prefix': 'config set',
                'who': 'osd',
                'name': 'bluestore_throttle_bytes',
                'value': self.get_module_option('throttle_bytes')
        })
        self.log.warning(f"Throttle enabled due to sustained high PSI on {exceeded_hosts} hosts")

    def throttle_off(self, below_hosts):
        self.last_throttle_removal = datetime.utcnow()
        self.set_store('last_throttle_removal', self.last_throttle_removal.isoformat())
        for item in reversed(self._throttle_history):
            if item["end"] is None:
                item["end"] = self.last_throttle_removal.isoformat()
                break
        self.set_store('throttle_history', json.dumps(self._throttle_history))
        self.set_store('throttle_enabled_time', None)
        self.throttle_enabled_time = None
        self.set_health_checks({'PSI_THROTTLE_ACTIVE': None})
        self.mon_command({
            'prefix': 'config rm',
            'who': 'osd',
            'name': 'bluestore_throttle_bytes'
        })
        self.log.info(f"Throttle removed: PSI dropped below thresholds on {below_hosts} hosts")

    @CLICommand('mgr psi snapshot', perm='r')
    def cli_snapshot(self):
        return 0, str(self._psi_snapshot), ''

    @CLICommand('mgr psi throttle_on', perm='w')
    def cli_throttle_on(self):
        if self.is_throttle_enabled():
            return -errno.EEXIST, '', 'Throttle is already enabled'
        self.throttle_on(exceeded_hosts=0)
        return 0, 'Throttle manually enabled', ''


    @CLICommand('mgr psi throttle_off', perm='w')
    def cli_throttle_off(self):
        if not self.is_throttle_enabled():
            return -errno.ENOENT, '', 'Throttle is not currently enabled'
        self.throttle_off(below_hosts=0)
        return 0, 'Throttle manually disabled', ''


    @CLICommand('mgr psi history', perm='r')
    def cli_throttle_history(self):
        return 0, json.dumps(self._throttle_history, indent=2), ''


    @CLICommand('mgr psi help', perm='r')
    def cli_help(self):
        cli_commands = [
            "mgr psi history            Show recent throttle history",
            "mgr psi throttle_on        Manually enable throttling",
            "mgr psi throttle_off       Manually disable throttling",
            "mgr psi snapshot           Retrieve the latest PSI sample",
            "mgr psi help               Show this help message"
        ]
        ceph_cmds = [
            "ceph -s",
            "  To check active MGR and throttling state in HEALTH dashboard",
            "ceph health detail -f json-pretty | less",
            "  To see more details of throttling",
            "ceph config get mgr mgr/psi/<option>",
            "  To get PSI option (see config section)",
            "ceph config set mgr mgr/psi/<option> <Value>",
            "  To set PSI option (see config section)",
            "ceph daemon <osd_id> psi",
            "  To get PSI probe from admin socket",
            "ceph config get osd bluestore_throttle_bytes",
            "  To check current throttling level",
        ]
        options = [
            f"{opt['name']:<25} ({opt['type']}): {opt.get('desc', '')} [default={opt.get('default')}]"
            for opt in self.MODULE_OPTIONS
        ]
        help_lines = [
            "PSI Module Usage:",
            "  This plugin collects PSI and CPU idle metrics from OSDs and adjusts throttling based on",
            "  cluster-wide pressure. It logs activity to:",
            "    /var/log/ceph/ceph-mgr.<mgr_id>.log  (filter with grep psi)",
            "",
            "  You can increase log verbosity with:",
            "    ceph config set mgr mgr/psi/log_level <debug|info|warning|error>",
            "",
            "CLI Commands:",
            *[f"  {cmd}" for cmd in cli_commands],
            "",
            "Options:",
            *[f"  {opt}" for opt in options],
            "",
            "Ceph Commands:",
            *[f"  {cmd}" for cmd in ceph_cmds],
            "Enable / Disable PSI plugin:",
            "  ceph mgr module enable psi",
            "    Enables PSI-based monitoring and automated throttling.",
            "  ceph mgr module disable psi",
            "    Disables PSI plugin. Current throttle state will be preserved and restored",
            "    automatically if the module is re-enabled later.",
        ]
        return 0, "\n".join(help_lines), ''

