#!/usr/bin/env python3
# Poll RAPL energy and other metrics every 10ms and write power_data.csv.

import time
import csv
import os
import signal
import threading

OUTPUT_PATH = "/home/danielhufnagle/Documents/CS446/MLPower/src/x86/power_data.csv"

stop_event = threading.Event()

def handle_signal(signum, frame):
    stop_event.set()

def get_rapl_energy(zone_path):
    try:
        with open(os.path.join(zone_path, "energy_uj"), "r") as f:
            return int(f.read())
    except:
        return None

def get_cpu_freq():
    try:
        with open("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", "r") as f:
            return int(f.read()) // 1000  # Convert to MHz
    except:
        return 0

def get_cpu_util():
    # Simplistic util: (non-idle / total) since last sample
    try:
        with open("/proc/stat", "r") as f:
            line = f.readline()
            parts = line.split()
            # user nice system idle iowait irq softirq steal guest guest_nice
            # 0    1    2      3    4      5   6       7     8     9
            idle = int(parts[4]) + int(parts[5])
            total = sum(int(p) for p in parts[1:])
            return total, idle
    except:
        return 0, 0

def get_ram_used():
    try:
        with open("/proc/meminfo", "r") as f:
            mem = {}
            for line in f:
                parts = line.split()
                mem[parts[0].replace(":", "")] = int(parts[1])
            return (mem['MemTotal'] - mem['MemAvailable']) // 1024
    except:
        return 0

def get_cpu_temp():
    # Find k10temp
    try:
        for hwmon in os.listdir("/sys/class/hwmon"):
            with open(f"/sys/class/hwmon/{hwmon}/name", "r") as f:
                if "k10temp" in f.read():
                    with open(f"/sys/class/hwmon/{hwmon}/temp1_input", "r") as f_temp:
                        return int(f_temp.read()) / 1000.0
    except:
        pass
    return 0.0

def main():
    signal.signal(signal.SIGINT,  handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    pkg_path = "/sys/class/powercap/intel-rapl:0"
    core_path = "/sys/class/powercap/intel-rapl:0/intel-rapl:0:0"

    print(f"collect_power: writing to {OUTPUT_PATH}", flush=True)

    with open(OUTPUT_PATH, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow([
            "timestamp_ns", "freq_mhz", "cpu_util_avg_pct", "emc_util_pct",
            "ram_used_mb", "cpu_temp_c", "tj_temp_c",
            "core_power_mw", "pkg_power_mw"
        ])
        csvfile.flush()

        last_ts = time.time_ns()
        last_pkg_energy = get_rapl_energy(pkg_path)
        last_core_energy = get_rapl_energy(core_path)
        last_total, last_idle = get_cpu_util()

        count = 0
        last_report_time = time.monotonic()

        while not stop_event.is_set():
            time.sleep(0.01)
            
            curr_ts = time.time_ns()
            curr_pkg_energy = get_rapl_energy(pkg_path)
            curr_core_energy = get_rapl_energy(core_path)
            curr_total, curr_idle = get_cpu_util()
            
            dt = (curr_ts - last_ts) / 1e9
            
            pkg_power = 0
            if curr_pkg_energy is not None and last_pkg_energy is not None:
                pkg_power = (curr_pkg_energy - last_pkg_energy) / (dt * 1000) # uJ to mW
            
            core_power = 0
            if curr_core_energy is not None and last_core_energy is not None:
                core_power = (curr_core_energy - last_core_energy) / (dt * 1000)
            
            util = 0
            if curr_total > last_total:
                util = 100.0 * (1.0 - (curr_idle - last_idle) / (curr_total - last_total))
            
            freq = get_cpu_freq()
            ram = get_ram_used()
            temp = get_cpu_temp()
            
            writer.writerow([
                curr_ts,
                freq,
                round(util, 1),
                0, # emc_util_pct not easily available on x86
                ram,
                temp,
                temp, # tj_temp_c use same as cpu_temp
                round(core_power, 1),
                round(pkg_power, 1)
            ])
            
            count += 1
            if count % 50 == 0:
                csvfile.flush()
            
            last_ts = curr_ts
            last_pkg_energy = curr_pkg_energy
            last_core_energy = curr_core_energy
            last_total, last_idle = curr_total, curr_idle
            
            now = time.monotonic()
            if now - last_report_time >= 30.0:
                print(f"collect_power: {count} rows written", flush=True)
                last_report_time = now

    print(f"collect_power: stopped — {count} rows written", flush=True)

if __name__ == "__main__":
    main()
