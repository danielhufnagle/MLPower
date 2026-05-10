#!/usr/bin/env python3
"""
collect_pmic.py - Poll system telemetry via tegrastats every 10ms.

Writes pmic_data.csv with columns:
    timestamp_ns, freq_khz, cpu_util_avg_pct, emc_util_pct,
    cpu_temp_c, tj_temp_c, cpu_gpu_cv_power_mw, vdd_in_power_mw

tegrastats is launched once and read line-by-line — no subprocess
spawning overhead per sample. Provides richer signal than raw INA3221
sysfs reads: per-core CPU utilization, EMC memory bandwidth utilization,
all thermal zones, and direct power readings in mW.

Stop with kill -2 <pid> or Ctrl+C.
"""

import time
import csv
import re
import signal
import subprocess
import threading

OUTPUT_PATH = "/home/mlpower/MLPower/src/data_collection/pmic_data.csv"

stop_event = threading.Event()

def handle_signal(signum, frame):
    stop_event.set()

def parse_tegrastats(line):
    """Extract relevant fields from one tegrastats output line."""
    result = {}

    # CPU [26%@1728,10%@1728,10%@1728,7%@1728,0%@1728,100%@1728]
    m = re.search(r'CPU \[([^\]]+)\]', line)
    if m:
        cores = m.group(1).split(',')
        utils = [int(c.split('%')[0]) for c in cores]
        result['cpu_util_avg'] = sum(utils) / len(utils)
        result['freq_mhz'] = int(cores[0].split('@')[1])  # all cores same freq

    # RAM 2562/7607MB
    m = re.search(r'RAM (\d+)/\d+MB', line)
    if m:
        result['ram_used_mb'] = int(m.group(1))

    # EMC_FREQ 0%@2133
    m = re.search(r'EMC_FREQ (\d+)%', line)
    if m:
        result['emc_util_pct'] = int(m.group(1))

    # cpu@50.312C
    m = re.search(r'cpu@([\d.]+)C', line)
    if m:
        result['cpu_temp_c'] = float(m.group(1))

    # tj@50.937C  (junction temp — max across all zones, throttling reference)
    m = re.search(r'tj@([\d.]+)C', line)
    if m:
        result['tj_temp_c'] = float(m.group(1))

    # VDD_CPU_GPU_CV 2641mW/avg — first value is current reading
    m = re.search(r'VDD_CPU_GPU_CV (\d+)mW', line)
    if m:
        result['cpu_gpu_cv_power_mw'] = int(m.group(1))

    # VDD_IN 6644mW/avg — total system input power
    m = re.search(r'VDD_IN (\d+)mW', line)
    if m:
        result['vdd_in_power_mw'] = int(m.group(1))

    return result

def main():
    signal.signal(signal.SIGINT,  handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print(f"collect_pmic: writing to {OUTPUT_PATH}")
    print("collect_pmic: stop with Ctrl+C or kill -2 <pid>")

    proc = subprocess.Popen(
        ['tegrastats', '--interval', '10'],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True
    )

    with open(OUTPUT_PATH, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow([
            "timestamp_ns", "freq_mhz", "cpu_util_avg_pct", "emc_util_pct",
            "ram_used_mb", "cpu_temp_c", "tj_temp_c",
            "cpu_gpu_cv_power_mw", "vdd_in_power_mw"
        ])

        count = 0
        for line in proc.stdout:
            if stop_event.is_set():
                break

            ts   = time.monotonic_ns()
            data = parse_tegrastats(line)

            if len(data) < 8:
                continue

            writer.writerow([
                ts,
                data['freq_mhz'],
                round(data['cpu_util_avg'], 1),
                data['emc_util_pct'],
                data['ram_used_mb'],
                data['cpu_temp_c'],
                data['tj_temp_c'],
                data['cpu_gpu_cv_power_mw'],
                data['vdd_in_power_mw'],
            ])
            count += 1

            if count % 100 == 0:
                csvfile.flush()

    proc.terminate()
    proc.wait()
    print(f"collect_pmic: stopped — {count} rows written")

if __name__ == "__main__":
    main()
