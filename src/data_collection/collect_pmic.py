#!/usr/bin/env python3
# Poll tegrastats every 10ms and write pmic_data.csv.

import time
import csv
import re
import signal
import subprocess
import threading

OUTPUT_PATH = "/home/mlpower/MLPower/src/data_collection/pmic_data.csv"

POWER_MISSING_THRESHOLD = 20  # consecutive missing rows before warning

stop_event = threading.Event()

def handle_signal(signum, frame):
    stop_event.set()

def parse_tegrastats(line):
    result = {}

    m = re.search(r'CPU \[([^\]]+)\]', line)
    if m:
        cores = m.group(1).split(',')
        utils = [int(c.split('%')[0]) for c in cores]
        result['cpu_util_avg'] = sum(utils) / len(utils)
        result['freq_mhz'] = int(cores[0].split('@')[1])

    m = re.search(r'RAM (\d+)/\d+MB', line)
    if m:
        result['ram_used_mb'] = int(m.group(1))

    m = re.search(r'EMC_FREQ (\d+)%', line)
    if m:
        result['emc_util_pct'] = int(m.group(1))

    m = re.search(r'cpu@([\d.]+)C', line)
    if m:
        result['cpu_temp_c'] = float(m.group(1))

    m = re.search(r'tj@([\d.]+)C', line)
    if m:
        result['tj_temp_c'] = float(m.group(1))

    m = re.search(r'VDD_CPU_GPU_CV (\d+)mW', line)
    if m:
        result['cpu_gpu_cv_power_mw'] = int(m.group(1))

    m = re.search(r'VDD_IN (\d+)mW', line)
    if m:
        result['vdd_in_power_mw'] = int(m.group(1))

    return result

def main():
    signal.signal(signal.SIGINT,  handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print(f"collect_pmic: writing to {OUTPUT_PATH}", flush=True)

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
        csvfile.flush()

        count            = 0
        skipped          = 0
        consecutive_skip = 0
        power_lost       = False
        last_report_time = time.monotonic()

        for line in proc.stdout:
            if stop_event.is_set():
                break

            ts   = time.monotonic_ns()
            data = parse_tegrastats(line)

            # VDD_IN and VDD_CPU_GPU_CV only present when INA3221 driver is bound
            has_power = ('vdd_in_power_mw' in data and
                         'cpu_gpu_cv_power_mw' in data)

            if not has_power:
                consecutive_skip += 1
                skipped          += 1

                if consecutive_skip == POWER_MISSING_THRESHOLD:
                    print(
                        f"\ncollect_pmic ERROR: power fields missing for "
                        f"{consecutive_skip} consecutive rows (~{consecutive_skip*10}ms).\n"
                        f"  INA3221 driver may have lost binding.\n"
                        f"  Run: echo 1-0040 | sudo tee "
                        f"/sys/bus/i2c/drivers/ina3221/bind\n"
                        f"  Rows written so far: {count}",
                        flush=True
                    )
                    power_lost = True

                if len(data) < 8:
                    continue

            else:
                if power_lost:
                    print(
                        f"collect_pmic: power fields restored after "
                        f"{consecutive_skip} skipped rows.",
                        flush=True
                    )
                    power_lost       = False
                consecutive_skip = 0

            if len(data) < 8:
                skipped += 1
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

            if count % 50 == 0:
                csvfile.flush()

            now = time.monotonic()
            if now - last_report_time >= 30.0:
                print(
                    f"collect_pmic: {count} rows written, "
                    f"{skipped} skipped",
                    flush=True
                )
                last_report_time = now

    proc.terminate()
    proc.wait()
    print(
        f"collect_pmic: stopped — {count} rows written, {skipped} skipped",
        flush=True
    )
    if skipped > 0:
        pct = 100.0 * skipped / max(count + skipped, 1)
        print(f"collect_pmic: skip rate was {pct:.1f}% — "
              f"{'INVESTIGATE' if pct > 5 else 'OK'}",
              flush=True)

if __name__ == "__main__":
    main()
