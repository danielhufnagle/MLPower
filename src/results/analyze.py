#!/usr/bin/env python3

import sys
import os
import csv
import glob
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

if len(sys.argv) < 2:
    print("Usage: python3 analyze.py <results_directory>")
    sys.exit(1)

RESULTS_DIR = sys.argv[1]
CSV_LOG = os.path.join(RESULTS_DIR, "telemetry.csv")

if not os.path.exists(CSV_LOG):
    print(f"Error: Telemetry CSV not found in {RESULTS_DIR}.")
    sys.exit(1)

print(f"Analyzing data in: {RESULTS_DIR}")

# --- Load CSV with stdlib only ---
rows = []
with open(CSV_LOG) as f:
    for row in csv.DictReader(f):
        rows.append({
            'governor': row['governor'],
            'workload':  row['workload'],
            'runtime_s':              float(row['runtime_s']),
            'avg_power_mw':           float(row['avg_power_mw']),
            'avg_temp_c':             float(row['avg_temp_c']),
            'energy_j':               float(row['energy_j']),
            'edp':                    float(row['edp']),
            'throughput':             float(row.get('throughput', 0) or 0),
            'throughput_per_joule':   float(row.get('throughput_per_joule', 0) or 0),
            'instructions_per_joule': float(row.get('instructions_per_joule', 0) or 0),
        })

# Aggregate (mean across reps) per governor+workload
from collections import defaultdict
sums   = defaultdict(lambda: defaultdict(float))
counts = defaultdict(lambda: defaultdict(int))
for r in rows:
    key = (r['governor'], r['workload'])
    for m in ('runtime_s', 'avg_power_mw', 'avg_temp_c', 'energy_j', 'edp', 'throughput', 'throughput_per_joule', 'instructions_per_joule'):
        sums[key][m]   += r[m]
        counts[key][m] += 1

agg = {}
for key, metrics in sums.items():
    agg[key] = {m: metrics[m] / counts[key][m] for m in metrics}

governors = sorted({r['governor'] for r in rows},
                   key=lambda g: ['performance','ondemand','schedutil','ml_governor'].index(g)
                   if g in ['performance','ondemand','schedutil','ml_governor'] else 99)
workloads = sorted({r['workload'] for r in rows})

# Write summary CSV
summary_path = os.path.join(RESULTS_DIR, "macro_summary.csv")
with open(summary_path, 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['governor','workload','runtime_s','avg_power_mw','avg_temp_c','energy_j','edp','throughput','throughput_per_joule','instructions_per_joule'])
    for (gov, wl), m in sorted(agg.items()):
        w.writerow([gov, wl, m['runtime_s'], m['avg_power_mw'], m['avg_temp_c'], m['energy_j'], m['edp'], m['throughput'], m['throughput_per_joule'], m['instructions_per_joule']])

# --- Plotting ---
plt.style.use('ggplot')
COLORS = ['#e41a1c','#377eb8','#4daf4a','#984ea3']
gov_color = {g: COLORS[i % len(COLORS)] for i, g in enumerate(governors)}

n_gov = len(governors)
width = 0.8 / n_gov

metrics = [
    ('avg_power_mw',           'Avg Power (mW)',                    'chart_power.png'),
    ('energy_j',               'Total Energy (J)',                  'chart_energy.png'),
    ('runtime_s',              'Runtime (s)',                       'chart_runtime.png'),
    ('edp',                    'Energy-Delay Product (J·s)',         'chart_edp.png'),
    ('throughput_per_joule',   'Throughput / Joule  (Higher is Better)',   'chart_tpj.png'),
    ('instructions_per_joule', 'Instructions / Joule  (Higher is Better)', 'chart_ipj.png'),
]

for metric, ylabel, fname in metrics:
    fig, ax = plt.subplots(figsize=(max(12, len(workloads) * 1.2), 6))
    x_base = range(len(workloads))
    for i, gov in enumerate(governors):
        offsets = [x + (i - n_gov/2 + 0.5) * width for x in x_base]
        values  = [agg.get((gov, wl), {}).get(metric, 0) for wl in workloads]
        ax.bar(offsets, values, width * 0.9, label=gov, color=gov_color[gov])
    ax.set_ylabel(ylabel)
    ax.set_title(ylabel)
    ax.set_xticks(list(x_base))
    ax.set_xticklabels(workloads, rotation=45, ha='right')
    ax.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(RESULTS_DIR, fname), dpi=150)
    plt.close()
    print(f"  saved {fname}")

# Frequency histograms per workload
for wl in workloads:
    fig, ax = plt.subplots(figsize=(10, 5))
    for gov in governors:
        freqs = []
        for d in glob.glob(os.path.join(RESULTS_DIR, f"{gov}_{wl}_run*")):
            fpath = os.path.join(d, "freq_trace.csv")
            if not os.path.exists(fpath):
                continue
            with open(fpath) as f:
                for line in f:
                    parts = line.strip().split(',')
                    if len(parts) == 2:
                        try:
                            v = float(parts[1])
                            if v > 0:
                                freqs.append(v / 1000.0)
                        except ValueError:
                            pass
        if freqs:
            ax.hist(freqs, bins=22, alpha=0.5, label=gov,
                    density=True, color=gov_color[gov], edgecolor='black')
    ax.set_title(f"Frequency Distribution — {wl}")
    ax.set_xlabel("Frequency (MHz)")
    ax.set_ylabel("Density")
    ax.legend()
    plt.tight_layout()
    fname = f"hist_freq_{wl}.png"
    plt.savefig(os.path.join(RESULTS_DIR, fname), dpi=150)
    plt.close()
    print(f"  saved {fname}")

print(f"\nDone. All figures saved to {RESULTS_DIR}/")
