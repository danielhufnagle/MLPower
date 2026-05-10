#!/usr/bin/env python3
"""
preprocess.py - Join PMU + PMIC data and engineer features for ML training.

Model inputs (87 total):
  - Current frequency:         freq_khz_p0, freq_khz_p4            =  2
  - Raw PMU deltas per core:   6 events + cycles x 6 cores         = 42
  - Derived ratios per core:   6 ratios x 6 cores                  = 36
  - PMIC via tegrastats daemon: util, emc, ram, temp, power         =  7

Targets:
  - label_freq_khz_p0, label_freq_khz_p4  (next-step governor decision)

Output: training_data.csv
"""

import pandas as pd
import numpy as np

PMU_PATH  = "pmu_data.csv"
PMIC_PATH = "pmic_data.csv"
OUT_PATH  = "training_data.csv"

CORES  = list(range(6))
EVENTS = ["inst_retired", "stall_backend", "stall_frontend",
          "ll_cache_miss_rd", "br_mis_pred", "dtlb_walk"]

# ── Load ──────────────────────────────────────────────────────────────────────

pmu  = pd.read_csv(PMU_PATH)
pmic = pd.read_csv(PMIC_PATH)

print(f"Loaded PMU:  {len(pmu):,} rows")
print(f"Loaded PMIC: {len(pmic):,} rows")

# ── Join on timestamp (nearest neighbour) ─────────────────────────────────────

pmu  = pmu.sort_values("timestamp_ns").reset_index(drop=True)
pmic = pmic.sort_values("timestamp_ns").reset_index(drop=True)

merged = pd.merge_asof(pmu, pmic, on="timestamp_ns", direction="nearest")

print(f"After join:  {len(merged):,} rows")

# ── Drop zero-cycle rows (core in deep WFI, PMU not counting) ─────────────────

cycle_cols = [f"cycles_c{c}" for c in CORES]
zero_mask  = (merged[cycle_cols] == 0).any(axis=1)
merged     = merged[~zero_mask].reset_index(drop=True)
print(f"After dropping zero-cycle rows: {len(merged):,} rows")

# ── Per-core derived ratios (clean floats) ────────────────────────────────────

for c in CORES:
    cyc  = merged[f"cycles_c{c}"].clip(lower=1).astype(float)
    inst = merged[f"inst_retired_c{c}"].clip(lower=1).astype(float)

    merged[f"stall_ratio_c{c}"]    = merged[f"stall_backend_c{c}"]    / cyc
    merged[f"fe_stall_ratio_c{c}"] = merged[f"stall_frontend_c{c}"]   / cyc
    merged[f"ipc_c{c}"]            = merged[f"inst_retired_c{c}"]     / cyc
    merged[f"ll_miss_rate_c{c}"]   = merged[f"ll_cache_miss_rd_c{c}"] / inst
    merged[f"br_misrate_c{c}"]     = merged[f"br_mis_pred_c{c}"]      / inst
    merged[f"dtlb_rate_c{c}"]      = merged[f"dtlb_walk_c{c}"]        / inst

# ── Clip ratios to physically meaningful bounds ───────────────────────────────
# Handles edge cases where a denominator was clipped to 1 (near-zero activity).
# stall ratios: can't exceed 1.0 (100% of cycles)
# ipc: Cortex-A78 is 4-wide, max IPC ~4
# per-instruction rates: cap at 1.0 (one event per instruction is already extreme)

for c in CORES:
    merged[f"stall_ratio_c{c}"]    = merged[f"stall_ratio_c{c}"].clip(0, 1.0)
    merged[f"fe_stall_ratio_c{c}"] = merged[f"fe_stall_ratio_c{c}"].clip(0, 1.0)
    merged[f"ipc_c{c}"]            = merged[f"ipc_c{c}"].clip(0, 4.0)
    merged[f"ll_miss_rate_c{c}"]   = merged[f"ll_miss_rate_c{c}"].clip(0, 1.0)
    merged[f"br_misrate_c{c}"]     = merged[f"br_misrate_c{c}"].clip(0, 1.0)
    merged[f"dtlb_rate_c{c}"]      = merged[f"dtlb_rate_c{c}"].clip(0, 1.0)

# ── Snap targets to nearest valid DVFS step ───────────────────────────────────
# cpufreq_get() returns transient values during frequency transitions.
# Hardcode the actual DVFS table from the Jetson Orin Nano cpufreq driver
# rather than inferring from data — avoids including transition artifacts.

valid_freqs_p0 = [
     115200,  192000,  268800,  345600,  422400,  499200,
     576000,  652800,  729600,  806400,  883200,  960000,
    1036800, 1113600, 1190400, 1267200, 1344000, 1420800,
    1497600, 1574400, 1651200, 1728000
]
valid_freqs_p4 = [
     115200,  192000,  268800,  345600,  422400,  499200,
     576000,  652800,  729600,  806400,  883200,  960000,
    1036800, 1113600, 1190400, 1267200, 1344000, 1420800,
    1497600, 1574400, 1651200, 1728000
]

def snap_to_nearest(val, valid):
    arr = np.array(valid)
    return arr[np.argmin(np.abs(arr - val))]

merged["label_freq_khz_p0"] = merged["freq_khz_p0"].shift(-1)
merged["label_freq_khz_p4"] = merged["freq_khz_p4"].shift(-1)
merged = merged.dropna(subset=["label_freq_khz_p0", "label_freq_khz_p4"])
merged["label_freq_khz_p0"] = merged["label_freq_khz_p0"].apply(
    lambda x: snap_to_nearest(x, valid_freqs_p0))
merged["label_freq_khz_p4"] = merged["label_freq_khz_p4"].apply(
    lambda x: snap_to_nearest(x, valid_freqs_p4))

# ── Column layout ─────────────────────────────────────────────────────────────

meta         = ["timestamp_ns"]
cur_freq     = ["freq_khz_p0", "freq_khz_p4"]
raw_pmu      = [f"{e}_c{c}" for c in CORES for e in EVENTS + ["cycles"]]
ratios       = [f"{r}_c{c}" for c in CORES
                for r in ["stall_ratio", "fe_stall_ratio", "ipc",
                           "ll_miss_rate", "br_misrate", "dtlb_rate"]]
pmic_inputs  = ["cpu_util_avg_pct", "emc_util_pct", "ram_used_mb",
                "cpu_temp_c", "tj_temp_c",
                "cpu_gpu_cv_power_mw", "vdd_in_power_mw"]
targets      = ["label_freq_khz_p0", "label_freq_khz_p4"]

all_cols = meta + cur_freq + raw_pmu + ratios + pmic_inputs + targets
merged   = merged[all_cols].dropna().reset_index(drop=True)

# ── Save ──────────────────────────────────────────────────────────────────────

merged.to_csv(OUT_PATH, index=False)

# ── Summary ───────────────────────────────────────────────────────────────────

n_inputs = len(cur_freq) + len(raw_pmu) + len(ratios) + len(pmic_inputs)
print(f"\nWrote {OUT_PATH}: {len(merged):,} rows x {len(merged.columns)} columns")
print(f"\nModel inputs: {n_inputs}")
print(f"  current frequency:  {len(cur_freq)}")
print(f"  raw PMU per-core:   {len(raw_pmu)}")
print(f"  ratios per-core:    {len(ratios)}")
print(f"  PMIC (via daemon):  {len(pmic_inputs)}")
print(f"\nTargets after snapping:")
print(f"  p0: {len(valid_freqs_p0)} valid steps, "
      f"{merged['label_freq_khz_p0'].nunique()} in dataset")
print(f"  p4: {len(valid_freqs_p4)} valid steps, "
      f"{merged['label_freq_khz_p4'].nunique()} in dataset")
print(f"\nClass balance (p0):")
counts = merged["label_freq_khz_p0"].value_counts()
print(f"  Most common:  {counts.index[0]//1000} MHz ({counts.iloc[0]} rows, "
      f"{100*counts.iloc[0]/len(merged):.1f}%)")
print(f"  Least common: {counts.index[-1]//1000} MHz ({counts.iloc[-1]} rows, "
      f"{100*counts.iloc[-1]/len(merged):.1f}%)")
print(f"  Imbalance ratio: {counts.iloc[0]/counts.iloc[-1]:.0f}x  "
      f"← use class weights during training")
print(f"\nPower range (RL reward signal): "
      f"{merged['cpu_gpu_cv_power_mw'].min():.0f}–"
      f"{merged['cpu_gpu_cv_power_mw'].max():.0f} mW")
