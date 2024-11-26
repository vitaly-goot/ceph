import subprocess
import re
import json
from collections import defaultdict

def get_ephemeral_port_range():
    try:
        # Open and read the file
        with open("/proc/sys/net/ipv4/ip_local_port_range", "r") as file:
            line = file.readline().strip()
            start, end = map(int, line.split())
        return range(start, end)
    except Exception as e:
        raise RuntimeError(f"Failed to read ephemeral port range: {e}")

def process_ss_output():
    # Run the `ss` command and capture its output
    result = subprocess.run(["ss", "-tnipaH"], capture_output=True, text=True)
    lines = result.stdout.strip().split("\n")

    # Combine every two lines into one
    combined_lines = [" ".join(lines[i:i+2]) for i in range(0, len(lines), 2)]

    # Initialize counters and accumulators
    ephemeral_count = 0
    non_ephemeral_count = 0
    state_counts = defaultdict(int)
    lastsnd_values = []

    # Define ephemeral port range
    ephemeral_range = get_ephemeral_port_range()

    # Process each concatenated line
    for line in combined_lines:
        if 'users:(("ceph-osd",' not in line:
            continue
        parts = line.split()
        if len(parts) < 5:
            continue


        # Extract state (first column)
        state = parts[0]
        state_counts[state] += 1

        # Extract local port from the 4th column
        local_addr_port = parts[3]
        port_match = re.search(r":(\d+)$", local_addr_port)
        if port_match:
            port = int(port_match.group(1))
            if port in ephemeral_range:
                ephemeral_count += 1
            else:
                non_ephemeral_count += 1

        # Extract lastsnd value
        lastsnd_match = re.search(r"lastsnd:(\d+)", line)
        if lastsnd_match:
            lastsnd_values.append(int(lastsnd_match.group(1)))

    # Calculate statistics for lastsnd
    lastsnd_count = len(lastsnd_values)
    lastsnd_max = max(lastsnd_values, default=0)
    lastsnd_avg = sum(lastsnd_values) / lastsnd_count if lastsnd_count > 0 else 0

    # Prepare output as JSON
    output = {
        "ephemeral_ports": ephemeral_count,
        "non_ephemeral_ports": non_ephemeral_count,
        "state_counts": dict(state_counts),
        "lastsnd_stats": {
            "count": lastsnd_count,
            "max": lastsnd_max,
            "average": round(lastsnd_avg, 2)
        }
    }

    return json.dumps(output)

# Usage
if __name__ == "__main__":
    print(process_ss_output())
