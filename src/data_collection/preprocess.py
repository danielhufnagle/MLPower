#!/usr/bin/env python3
"""
preprocess.py — Join PMU + PMIC data, K-means labeling, write training samples.

Usage:
    python3 preprocess.py            # auto-select K via elbow
    python3 preprocess.py --K 40     # pin K
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

# All 22 valid DVFS steps on the Jetson Orin Nano (kHz)
DVFS_STEPS = np.array([
    115200,  192000,  268800,  345600,  422400,  499200,
    576000,  652800,  729600,  806400,  883200,  960000,
    1036800, 1113600, 1190400, 1267200, 1344000, 1420800,
    1497600, 1574400, 1651200, 1728000,
], dtype=np.int64)

PMU_EVENTS = ['inst_retired', 'stall_backend', 'stall_frontend',
              'll_cache_miss_rd', 'br_mis_pred', 'dtlb_walk']
N_CORES    = 6
RATIO_COLS = ['stall_ratio', 'fe_stall_ratio', 'ipc',
              'll_miss_rate', 'br_misrate', 'dtlb_rate']
PMIC_COLS  = ['cpu_util_avg_pct', 'emc_util_pct', 'ram_used_mb',
              'cpu_temp_c', 'tj_temp_c', 'cpu_gpu_cv_power_mw', 'vdd_in_power_mw']

RATIO_CLIP = {
    'stall_ratio':    (0.0, 1.0),
    'fe_stall_ratio': (0.0, 1.0),
    'ipc':            (0.0, 5.0),
    'll_miss_rate':   (0.0, 0.5),
    'br_misrate':     (0.0, 0.5),
    'dtlb_rate':      (0.0, 0.5),
}

# Benchmarks that vary in workload
MIXED_BENCHMARKS = set({'multiphase', 'bursty', 'ramp'})

FEATURE_COLS = (
    ['freq_khz_p0', 'freq_khz_p4']
    + [f'{ev}_c{c}' for ev in PMU_EVENTS + ['cycles'] for c in range(N_CORES)]
    + [f'{r}_c{c}'  for r  in RATIO_COLS              for c in range(N_CORES)]
    + PMIC_COLS
)
assert len(FEATURE_COLS) == 87, f"expected 87 features, got {len(FEATURE_COLS)}"

# ── load ──────────────────────────────────────────────────────────────────────

def load(data_dir):
    pmu  = pd.read_csv(data_dir / 'pmu_data.csv')
    pmic = pd.read_csv(data_dir / 'pmic_data.csv')
    segs = pd.read_csv(data_dir / 'segments.csv')
    print(f"loaded:  {len(pmu):,} PMU rows  {len(pmic):,} PMIC rows  {len(segs)} segments")

    # Filter out rows with NaN
    pmu  = pmu.dropna()
    pmic = pmic.dropna(subset=['vdd_in_power_mw', 'cpu_gpu_cv_power_mw'])

    # If IPC greater than 6, drop the row. Signals that there has been overflow. 
    before = len(pmu)
    for c in range(N_CORES):
        ipc_raw = pmu[f'inst_retired_c{c}'] / pmu[f'cycles_c{c}'].clip(lower=1)
        pmu = pmu[ipc_raw <= 6.0]
    dropped = before - len(pmu)
    if dropped:
        print(f"  dropped {dropped} rows with PMU counter overflow (IPC > 6.0)")

    print(f"cleaned: {len(pmu):,} PMU rows  {len(pmic):,} PMIC rows")
    return pmu, pmic, segs

# Convert segments timesteps to monotonic (in line with pmu and pmic csvs) 
def realtime_to_monotonic(segs):
    offset = time.time_ns() - time.monotonic_ns()
    segs   = segs.copy()
    segs['start_ns'] -= offset
    segs['end_ns']   -= offset
    return segs

# ── segment tagging ───────────────────────────────────────────────────────────

def tag_segments(df, segs):
    """
    For each PMU row, find which segment it falls in.
    Rows outside all segments (cooldown, settle periods) are dropped.
    """
    df   = df.sort_values('timestamp_ns').reset_index(drop=True)
    segs = segs.sort_values('start_ns').reset_index(drop=True)

    ts         = df['timestamp_ns'].values
    seg_starts = segs['start_ns'].values
    seg_ends   = segs['end_ns'].values

    # For each timestamp, find the index of the last segment that started ≤ ts
    idx     = np.searchsorted(seg_starts, ts, side='right') - 1
    clamped = np.clip(idx, 0, len(segs) - 1)
    in_seg  = (idx >= 0) & (ts <= seg_ends[clamped])

    df      = df[in_seg].copy()
    clamped = clamped[in_seg]

    df['segment_id'] = segs['segment_id'].values[clamped]
    df['benchmark']  = segs['benchmark'].values[clamped]
    df['freq_khz']   = segs['freq_khz'].values[clamped]   # 0 for Phase 2
    df['governor']   = segs['governor'].values[clamped]
    df['phase']      = np.where(df['governor'] == 'userspace', 1, 2)

    n1 = (df['phase'] == 1).sum()
    n2 = (df['phase'] == 2).sum()
    print(f"inside segments: {len(df):,} rows  (phase1={n1:,}  phase2={n2:,})")
    return df

# ── ratio features ────────────────────────────────────────────────────────────

def compute_ratios(df):
    df = df.copy()
    for c in range(N_CORES):
        cyc  = df[f'cycles_c{c}'].clip(lower=1).astype(float)
        inst = df[f'inst_retired_c{c}'].clip(lower=1).astype(float)

        lo, hi = RATIO_CLIP['stall_ratio']
        df[f'stall_ratio_c{c}']    = (df[f'stall_backend_c{c}']    / cyc ).clip(lo, hi)
        lo, hi = RATIO_CLIP['fe_stall_ratio']
        df[f'fe_stall_ratio_c{c}'] = (df[f'stall_frontend_c{c}']   / cyc ).clip(lo, hi)
        lo, hi = RATIO_CLIP['ipc']
        df[f'ipc_c{c}']            = (df[f'inst_retired_c{c}']     / cyc ).clip(lo, hi)
        lo, hi = RATIO_CLIP['ll_miss_rate']
        df[f'll_miss_rate_c{c}']   = (df[f'll_cache_miss_rd_c{c}'] / inst).clip(lo, hi)
        lo, hi = RATIO_CLIP['br_misrate']
        df[f'br_misrate_c{c}']     = (df[f'br_mis_pred_c{c}']      / inst).clip(lo, hi)
        lo, hi = RATIO_CLIP['dtlb_rate']
        df[f'dtlb_rate_c{c}']      = (df[f'dtlb_walk_c{c}']        / inst).clip(lo, hi)
    return df

# ── PMU × PMIC join ───────────────────────────────────────────────────────────

def join_pmic(pmu, pmic):
    """
    Each PMU row (~16ms) gets merged with the nearest PMIC row (~10ms) based on timestamp.
    tolerance=100ms: PMU rows with no PMIC within 100ms are dropped
    """
    pmu  = pmu.sort_values('timestamp_ns')
    pmic = pmic.sort_values('timestamp_ns')

    merged = pd.merge_asof(
        pmu,
        pmic[['timestamp_ns'] + PMIC_COLS],
        on='timestamp_ns',
        direction='nearest',
        tolerance=100_000_000,
    )
    before = len(merged)
    merged = merged.dropna(subset=['vdd_in_power_mw'])
    dropped = before - len(merged)
    if dropped:
        print(f"PMU-PMIC join: dropped {dropped} rows (no PMIC within 100ms)")
    print(f"joined: {len(merged):,} rows")
    return merged


def compute_energy(df):
    """
    Add energy_mj column: vdd_in_power_mw × window_duration.
    mj = microjoules
    """
    df = df.sort_values(['segment_id', 'timestamp_ns']).copy()
    dt = df.groupby('segment_id')['timestamp_ns'].diff(periods=-1).abs()
    # Fill the last row of each segment with that segment's median Δt
    seg_median = dt.groupby(df['segment_id']).transform('median')
    dt = dt.fillna(seg_median)
    df['energy_mj'] = df['vdd_in_power_mw'] * dt / 1e6
    print(f"  mean window Δt: {dt.mean()/1e6:.2f} ms  std: {dt.std()/1e6:.2f} ms")
    return df

# ── K-means clustering ─────────────────────────────────────────────────

def _kmeans_X(df):
    """38 features used for k-means clustering.

    36 PMU ratios (6 per core) + cpu_util_avg_pct + emc_util_pct (mem utilization)  
    """
    ratio_cols = [f'{r}_c{c}' for r in RATIO_COLS for c in range(N_CORES)]
    return df[ratio_cols + ['emc_util_pct', 'cpu_util_avg_pct']].values.astype(np.float32)


def _kneedle(ks, inertias):
    """Determine optimal K -- The Elbow Method"""
    ks  = np.array(ks,       dtype=float)
    ins = np.array(inertias, dtype=float)
    ks_n  = (ks  - ks.min())  / (ks.max()  - ks.min())
    ins_n = (ins - ins.min()) / (ins.max() - ins.min())
    dists = np.abs(ins_n + ks_n - 1) / np.sqrt(2)
    return int(ks[np.argmax(dists)])


def select_k(Xs, k_min=10, k_max=80, k_step=5):
    """
    Run MiniBatchKMeans for K=10,15,...,80 on Phase 1 data.
    Returns best k value. 
    """
    bs = min(10_000, len(Xs))
    ks, inertias = [], []
    print(f"  elbow search K={k_min}..{k_max} step={k_step} on {len(Xs):,} points:")
    for k in range(k_min, k_max + 1, k_step):
        km = MiniBatchKMeans(n_clusters=k, n_init=3, batch_size=bs, random_state=42)
        km.fit(Xs)
        ks.append(k)
        inertias.append(km.inertia_)
        print(f"    K={k:3d}  inertia={km.inertia_:>12.1f}")

    best_k = _kneedle(ks, inertias)
    print(f"  → elbow at K={best_k}")
    return best_k


def fit_kmeans(Xs, K):
    print(f"K-means K={K} on {len(Xs):,} Phase 1 rows...")
    km = KMeans(n_clusters=K, n_init=10, random_state=42) # 42 because it is the answer to the universe
    km.fit(Xs)
    print(f"  inertia={km.inertia_:.1f}")
    return km


def compute_f_star(phase1, km, scaler):
    """
    For each cluster, find the frequency that
    maximizes aggregate efficiency = sum(instructions) / sum(energy_mj).
    """
    Xs      = scaler.transform(_kmeans_X(phase1))
    cluster = km.predict(Xs)

    p1 = phase1.copy()
    p1['cluster'] = cluster

    inst_cols        = [f'inst_retired_c{c}' for c in range(N_CORES)]
    p1['total_inst'] = p1[inst_cols].sum(axis=1)

    pure = p1[~p1['benchmark'].isin(MIXED_BENCHMARKS)]

    agg = (pure.groupby(['cluster', 'freq_khz'])
               .agg(total_inst=('total_inst', 'sum'),
                    total_energy=('energy_mj',  'sum'),
                    n=('total_inst', 'count'))
               .reset_index())
    agg['agg_eff'] = agg['total_inst'] / agg['total_energy'].clip(lower=1e-9)

    MIN_CELL = 20
    eff = agg[agg['n'] >= MIN_CELL].copy()

    all_clusters  = set(range(km.n_clusters))
    pure_clusters = set(eff['cluster'].unique())
    missing       = all_clusters - pure_clusters
    if missing:
        agg_fb = (p1[p1['cluster'].isin(missing)]
                    .groupby(['cluster', 'freq_khz'])
                    .agg(total_inst=('total_inst', 'sum'),
                         total_energy=('energy_mj',  'sum'),
                         n=('total_inst', 'count'))
                    .reset_index())
        agg_fb['agg_eff'] = agg_fb['total_inst'] / agg_fb['total_energy'].clip(lower=1e-9)
        agg_fb = agg_fb[agg_fb['n'] >= 5]
        eff = pd.concat([eff, agg_fb], ignore_index=True)

    best   = eff.loc[eff.groupby('cluster')['agg_eff'].idxmax()]
    f_star = best.set_index('cluster')['freq_khz'].astype(int).to_dict()

    mean_ipc = p1.groupby('cluster')[[f'ipc_c{c}' for c in range(N_CORES)]].mean().mean(axis=1)
    for k, ipc in mean_ipc.items():
        if ipc < 0.05:
            f_star[k] = int(DVFS_STEPS[0])

    print("F*(cluster) — efficiency-optimal frequency per regime:")
    for k in sorted(f_star):
        n         = (p1['cluster'] == k).sum()
        ipc       = mean_ipc[k]
        tag       = " [idle→min]" if ipc < 0.05 else ""
        e_rows    = eff[(eff['cluster'] == k) & (eff['freq_khz'] == f_star[k])]['agg_eff']
        e         = e_rows.values[0] if len(e_rows) else float('nan')
        top_bench = p1[p1['cluster'] == k]['benchmark'].value_counts().index[0]
        print(f"  cluster {k:2d}: {f_star[k]:>8} kHz  n={n:>5,}  eff={e:>10.1f}"
              f"  ipc={ipc:.3f}  top={top_bench}{tag}")

    return f_star


def label_rows(df, km, scaler, f_star):
    Xs       = scaler.transform(_kmeans_X(df))
    clusters = km.predict(Xs)

    freq_to_cls = {int(f): i for i, f in enumerate(DVFS_STEPS)}

    df = df.copy()
    df['cluster']        = clusters
    df['label_freq_khz'] = pd.Series(clusters, index=df.index).map(f_star)

    missing = df['label_freq_khz'].isna().sum()
    if missing:
        # Should not happen given the fallback in compute_f_star, but guard anyway.
        fallback = int(DVFS_STEPS[len(DVFS_STEPS) // 2])
        df['label_freq_khz'].fillna(fallback, inplace=True)
        print(f"  WARNING: {missing} rows had no f_star entry, filled with {fallback} kHz")

    # If IPC less than 5% for a row, assign lowest frequency as label
    idle_mask = df['cpu_util_avg_pct'] < 5.0
    n_idle    = idle_mask.sum()
    if n_idle:
        df.loc[idle_mask, 'label_freq_khz'] = int(DVFS_STEPS[0])
        print(f"  idle override: {n_idle:,} rows (cpu_util < 5%) → {DVFS_STEPS[0]:,} kHz")

    df['label_class'] = df['label_freq_khz'].map(freq_to_cls).astype(int)

    print("label distribution:")
    for freq, cnt in df['label_freq_khz'].value_counts().sort_index().items():
        print(f"  {int(freq):>8} kHz  {cnt:>7,}  ({100*cnt/len(df):.1f}%)")
    return df

# ── main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--K',        default='auto',
                    help='K-means clusters, or "auto" to select via elbow (default: auto)')
    ap.add_argument('--data-dir', type=Path, default=DATA_DIR)
    args = ap.parse_args()

    pmu, pmic, segs = load(args.data_dir)

    print("\n-- clock alignment --")
    segs = realtime_to_monotonic(segs)

    print("\n-- segment filtering --")
    pmu = tag_segments(pmu, segs)

    print("\n-- ratio features --")
    pmu = compute_ratios(pmu)

    print("\n-- PMU-PMIC join --")
    df = join_pmic(pmu, pmic)

    print("\n-- energy computation --")
    df = compute_energy(df)

    phase1 = df[df['phase'] == 1]
    phase2 = df[df['phase'] == 2]
    print(f"phase1={len(phase1):,}  phase2={len(phase2):,}")

    print("\n-- scaling --")
    X_p1   = _kmeans_X(phase1)
    scaler = StandardScaler()
    Xs_p1  = scaler.fit_transform(X_p1)

    print("\n-- K selection --")
    if args.K == 'auto':
        K = select_k(Xs_p1)
    else:
        K = int(args.K)
        print(f"  K={K} (pinned)")

    print("\n-- K-means --")
    km     = fit_kmeans(Xs_p1, K)
    f_star = compute_f_star(phase1, km, scaler)

    print("\n-- labeling --")
    df = label_rows(df, km, scaler, f_star)

    print("\n-- saving --")
    # Snap measured freq to nearest valid DVFS step before writing — guards
    # against rare transient cpufreq readings during governor transitions.
    for col in ['freq_khz_p0', 'freq_khz_p4']:
        vals    = df[col].values[:, np.newaxis]
        df[col] = DVFS_STEPS[np.argmin(np.abs(DVFS_STEPS - vals), axis=1)]

    out      = df[FEATURE_COLS].copy().astype(np.float32)
    out['label_class'] = df['label_class'].values.astype(np.int32)
    out['segment_id']  = df['segment_id'].values.astype(np.int32)

    out_path = args.data_dir / 'training_data.csv'
    out.to_csv(out_path, index=False)
    print(f"training_data.csv: {len(out):,} rows × {len(FEATURE_COLS)} features + label")

    manifest = args.data_dir / 'feature_cols.txt'
    manifest.write_text('\n'.join(FEATURE_COLS))
    print(f"feature_cols.txt:  {len(FEATURE_COLS)} column names saved")

    print("\n-- class distribution --")
    y = df['label_class'].values
    counts = pd.Series(y).value_counts().sort_index()
    for cls, cnt in counts.items():
        freq = DVFS_STEPS[cls]
        print(f"  class {cls:2d}  {freq:>8} kHz  {cnt:>6,}  ({100*cnt/len(y):.1f}%)")
    imbalance = counts.max() / counts.min()
    print(f"  imbalance ratio: {imbalance:.0f}x")


if __name__ == '__main__':
    main()
