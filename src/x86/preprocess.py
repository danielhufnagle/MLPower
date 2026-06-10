#!/usr/bin/env python3
"""
preprocess.py for x86 (AMD).
Join PMU + Power data, K-means labeling, write training samples.
"""

import argparse
import time
from pathlib import Path
import numpy as np
import pandas as pd
from sklearn.cluster import KMeans, MiniBatchKMeans
from sklearn.preprocessing import StandardScaler

# ── constants ─────────────────────────────────────────────────────────────────

DATA_DIR = Path(__file__).parent

# x86 Sweep frequencies
DVFS_STEPS = np.array([
    400000, 800000, 1200000, 1600000, 2000000, 2400000, 
    2800000, 3200000, 3600000, 4000000, 4400000, 4800000
], dtype=np.int64)

PMU_EVENTS = ['inst_retired', 'stall_backend', 'stall_frontend',
              'l2_cache_miss', 'br_mis_pred', 'dtlb_miss']
N_CORES    = 12 # Adjust if needed
RATIO_COLS = ['stall_ratio', 'fe_stall_ratio', 'ipc',
              'l2_miss_rate', 'br_misrate', 'dtlb_rate']
PMIC_COLS  = ['cpu_util_avg_pct', 'emc_util_pct', 'ram_used_mb',
              'cpu_temp_c', 'tj_temp_c', 'core_power_mw', 'pkg_power_mw']

RATIO_CLIP = {
    'stall_ratio':    (0.0, 1.0),
    'fe_stall_ratio': (0.0, 1.0),
    'ipc':            (0.0, 5.0),
    'l2_miss_rate':   (0.0, 0.5),
    'br_misrate':     (0.0, 0.5),
    'dtlb_rate':      (0.0, 0.5),
}

MIXED_BENCHMARKS = set() # Add if needed

FEATURE_COLS = (
    ['freq_khz_p0', 'freq_khz_p6']
    + [f'{ev}_c{c}' for ev in PMU_EVENTS + ['cycles'] for c in range(N_CORES)]
    + [f'{r}_c{c}'  for r  in RATIO_COLS              for c in range(N_CORES)]
    + PMIC_COLS
)

# ── load ──────────────────────────────────────────────────────────────────────

def load(data_dir):
    pmu  = pd.read_csv(data_dir / 'pmu_data.csv')
    pmic = pd.read_csv(data_dir / 'power_data.csv')
    segs = pd.read_csv(data_dir / 'segments.csv')
    print(f"loaded:  {len(pmu):,} PMU rows  {len(pmic):,} Power rows  {len(segs)} segments")
    return pmu, pmic, segs

def tag_segments(df, segs):
    df   = df.sort_values('timestamp_ns').reset_index(drop=True)
    segs = segs.sort_values('start_ns').reset_index(drop=True)
    ts         = df['timestamp_ns'].values
    seg_starts = segs['start_ns'].values
    seg_ends   = segs['end_ns'].values
    idx     = np.searchsorted(seg_starts, ts, side='right') - 1
    clamped = np.clip(idx, 0, len(segs) - 1)
    in_seg  = (idx >= 0) & (ts <= seg_ends[clamped])
    df      = df[in_seg].copy()
    clamped = clamped[in_seg]
    df['segment_id'] = segs['segment_id'].values[clamped]
    df['benchmark']  = segs['benchmark'].values[clamped]
    df['freq_khz']   = segs['freq_khz'].values[clamped]
    df['governor']   = segs['governor'].values[clamped]
    df['phase']      = np.where(df['governor'] == 'passive', 1, 2)
    return df

def compute_ratios(df):
    df = df.copy()
    for c in range(N_CORES):
        cyc  = df[f'cycles_c{c}'].clip(lower=1).astype(float)
        inst = df[f'inst_retired_c{c}'].clip(lower=1).astype(float)
        df[f'stall_ratio_c{c}']    = (df[f'stall_backend_c{c}']    / cyc ).clip(*RATIO_CLIP['stall_ratio'])
        df[f'fe_stall_ratio_c{c}'] = (df[f'stall_frontend_c{c}']   / cyc ).clip(*RATIO_CLIP['fe_stall_ratio'])
        df[f'ipc_c{c}']            = (df[f'inst_retired_c{c}']     / cyc ).clip(*RATIO_CLIP['ipc'])
        df[f'l2_miss_rate_c{c}']   = (df[f'l2_cache_miss_c{c}']    / inst).clip(*RATIO_CLIP['l2_miss_rate'])
        df[f'br_misrate_c{c}']     = (df[f'br_mis_pred_c{c}']      / inst).clip(*RATIO_CLIP['br_misrate'])
        df[f'dtlb_rate_c{c}']      = (df[f'dtlb_miss_c{c}']        / inst).clip(*RATIO_CLIP['dtlb_rate'])
    return df

def join_power(pmu, pmic):
    pmu  = pmu.sort_values('timestamp_ns')
    pmic = pmic.sort_values('timestamp_ns')
    merged = pd.merge_asof(
        pmu,
        pmic[['timestamp_ns'] + PMIC_COLS],
        on='timestamp_ns',
        direction='nearest',
        tolerance=100_000_000,
    )
    return merged.dropna(subset=['pkg_power_mw'])

def compute_energy(df):
    df = df.sort_values(['segment_id', 'timestamp_ns']).copy()
    dt = df.groupby('segment_id')['timestamp_ns'].diff(periods=-1).abs()
    seg_median = dt.groupby(df['segment_id']).transform('median')
    dt = dt.fillna(seg_median)
    df['energy_mj'] = df['pkg_power_mw'] * dt / 1e6
    return df

def _kmeans_X(df):
    ratio_cols = [f'{r}_c{c}' for r in RATIO_COLS for c in range(N_CORES)]
    return df[ratio_cols + ['cpu_util_avg_pct']].values.astype(np.float32)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data-dir', type=Path, default=DATA_DIR)
    args = ap.parse_args()

    pmu, pmic, segs = load(args.data_dir)
    pmu = tag_segments(pmu, segs)
    pmu = compute_ratios(pmu)
    df = join_power(pmu, pmic)
    df = compute_energy(df)

    # Phase 1: determine labels via clustering
    # Include all governors to ensure we have data, especially in short test runs
    phase1 = df[df['governor'].isin(['passive', 'performance', 'powersave'])].copy()
    X_p1   = _kmeans_X(phase1)
    scaler = StandardScaler()
    Xs_p1  = scaler.fit_transform(X_p1)

    n_clusters = 20
    km = KMeans(n_clusters=n_clusters, n_init=10, random_state=42)
    phase1['cluster'] = km.fit_predict(Xs_p1)

    # For each cluster, find the frequency that minimizes energy_mj
    # We'll look at the median energy for each (cluster, frequency)
    cluster_freq_stats = phase1.groupby(['cluster', 'freq_khz'])['energy_mj'].median().reset_index()
    
    f_star = {}
    for c in range(n_clusters):
        subset = cluster_freq_stats[cluster_freq_stats['cluster'] == c]
        if len(subset) == 0:
            f_star[c] = DVFS_STEPS[-1] # Default to max
            continue
        best_freq = subset.loc[subset['energy_mj'].idxmin(), 'freq_khz']
        f_star[c] = best_freq
        print(f"Cluster {c:2d}: best_freq = {best_freq:7d} kHz")

    # Now label all data (Phase 1 and Phase 2)
    # We need to predict the cluster for Phase 2 data as well
    X_all  = _kmeans_X(df)
    Xs_all = scaler.transform(X_all)
    df['cluster'] = km.predict(Xs_all)
    df['label_freq'] = df['cluster'].map(f_star)
    
    # Map frequency to class index
    freq_to_idx = {f: i for i, f in enumerate(DVFS_STEPS)}
    df['label_class'] = df['label_freq'].map(freq_to_idx)

    # Drop samples with NaN labels (shouldn't happen if f_star is complete)
    df = df.dropna(subset=['label_class'])
    df['label_class'] = df['label_class'].astype(int)

    out_path = args.data_dir / 'training_data.csv'
    df.to_csv(out_path, index=False)
    print(f"saved joined and labeled data to {out_path} ({len(df):,} rows)")

if __name__ == '__main__':
    main()
