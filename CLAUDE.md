# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MLPower is an ML-based power management system for Linux targeting the NVIDIA Jetson Orin Nano (aarch64). The project collects hardware performance data via ARM PMU kernel modules to train ML models for intelligent CPU frequency scaling.

**Goal:** Replace the schedutil governor with an ML model that recognizes memory-bound vs compute-bound workloads and scales frequency accordingly — boosting only when higher frequency actually helps, saving power otherwise.

**Authors:** Novel Alam, Daniel Hufnagle, William McGarry

## Build Commands

```bash
# Build a module
cd src/<module_dir>
make

# Clean build artifacts
make clean

# Load / unload
sudo insmod <module>.ko
sudo rmmod <module>

# Check kernel log output
dmesg | tail
```

## Architecture

**Platform:** aarch64 (ARM64) — NVIDIA Jetson Orin Nano. Code must be compatible with this architecture. `src/check_cpu_freq` is the only module that also works on x86.

**Kernel module build system:** Standard Linux kbuild. Each Makefile compiles against `/lib/modules/$(shell uname -r)/build/`.

**cpufreq policies:** Two frequency domains on the Jetson Orin Nano:
- `policy0` — cores 0–3, frequency read via `cpufreq_get(0)`
- `policy4` — cores 4–5, frequency read via `cpufreq_get(4)`

**Valid DVFS steps (both policies):** 115200, 192000, 268800, 345600, 422400, 499200, 576000, 652800, 729600, 806400, 883200, 960000, 1036800, 1113600, 1190400, 1267200, 1344000, 1420800, 1497600, 1574400, 1651200, 1728000 kHz

**nvpmodel:** Must be set to MAXN_SUPER (mode 2) for full frequency range:
```bash
sudo nvpmodel -m 2
```
Default mode 1 (25W) caps at 1344 MHz. Mode 0 (15W) caps at 1497 MHz.

**Current modules:**

| Directory | Module | Purpose |
|---|---|---|
| `src/test_module/` | `test` | Proof-of-concept validating the build pipeline |
| `src/check_cpu_freq/` | `check_cpu_frequency` | Polls `cpufreq_get(0)` every 4 ms via kthread; logs frequency to dmesg |
| `src/cache_miss_counting/` | *(prototype)* | Early ARM PMU experiment: counts L1D cache refills over a 5 s window |
| `src/pmu_profiler/` | `pmu_profiler` | Original PMU profiler — development/reference copy |
| `src/data_collection/` | `pmu_profiler` | Production data collection — all-core sampling, PMIC poller, benchmark orchestration |

---

## data_collection (src/data_collection)

Production data collection pipeline. Samples all 6 CPU cores simultaneously via IPI every ~16ms.

### Files

- `pmu_profiler.c` — kernel module; samples all 6 cores via `on_each_cpu()` IPI every ~16ms
- `collect_pmic.py` — userspace Python script; polls tegrastats every 10ms
- `collect.sh` — orchestration script; run as root to collect a full dataset
- `preprocess.py` — joins pmu_data.csv + pmic_data.csv, engineers features, outputs training_data.csv
- `Makefile` — standard kbuild

### Collecting data

```bash
cd src/data_collection
make
sudo bash ./collect.sh
```

Produces `pmu_data.csv` and `pmic_data.csv`. To preserve a run before collecting again:
```bash
mv pmu_data.csv pmu_data_run1.csv
mv pmic_data.csv pmic_data_run1.csv
```

Multiple runs must be manually combined before preprocessing (see below).

### Preprocessing

```bash
python3 preprocess.py
```

Reads `pmu_data.csv` + `pmic_data.csv`, outputs `training_data.csv`.

To combine multiple runs first:
```python
import pandas as pd
pmu  = pd.concat([pd.read_csv('pmu_data_run1.csv'),  pd.read_csv('pmu_data.csv')]).sort_values('timestamp_ns')
pmic = pd.concat([pd.read_csv('pmic_data_run1.csv'), pd.read_csv('pmic_data.csv')]).sort_values('timestamp_ns')
pmu.to_csv('pmu_data.csv', index=False)
pmic.to_csv('pmic_data.csv', index=False)
```
Then run `python3 preprocess.py` normally.

### PMU events (6 hardware slots)

- Slot 0 — `INST_RETIRED` (0x08): instructions retired — baseline throughput
- Slot 1 — `STALL_BACKEND` (0x24): cycles CPU blocked waiting for data — key scaling signal
- Slot 2 — `STALL_FRONTEND` (0x23): cycles CPU blocked waiting for instructions
- Slot 3 — `LL_CACHE_MISS_RD` (0x37): demand reads reaching DRAM — root cause of backend stalls
- Slot 4 — `BR_MIS_PRED` (0x10): branch mispredictions — pipeline flush pressure
- Slot 5 — `DTLB_WALK` (0x34): data TLB page-table walks — scattered memory access pressure
- `PMCCNTR_EL0`: cycle counter (read separately, not a slot)

Sampling interval: ~16ms (3 jiffies at CONFIG_HZ=250 + ~4ms work function overhead). `msecs_to_jiffies(10)` is used; true 10ms would require hrtimer refactor.

### pmu_data.csv columns

```
timestamp_ns, freq_khz_p0, freq_khz_p4,
<event>_c0 ... <event>_c5  (6 events + cycles per core, 42 columns)
```
Deltas over each ~16ms sampling window. All 6 cores captured simultaneously via IPI.

### pmic_data.csv columns

```
timestamp_ns, freq_mhz, cpu_util_avg_pct, emc_util_pct,
ram_used_mb, cpu_temp_c, tj_temp_c, cpu_gpu_cv_power_mw, vdd_in_power_mw
```
Sampled every ~10ms via tegrastats subprocess.

- `cpu_gpu_cv_power_mw` — CPU+GPU+CV combined rail (no separate CPU-only rail on INA3221)
- `vdd_in_power_mw` — total system input power (RL reward signal)

Timestamps: both use `CLOCK_MONOTONIC` (`ktime_get_ns()` in kernel, `time.monotonic_ns()` in Python) — joinable by timestamp.

### training_data.csv columns (output of preprocess.py)

90 columns total:
- `timestamp_ns` — meta
- `freq_khz_p0`, `freq_khz_p4` — current frequency (model input)
- Raw PMU deltas per core: 6 events + cycles × 6 cores = 42 columns
- Derived ratios per core: `stall_ratio`, `fe_stall_ratio`, `ipc`, `ll_miss_rate`, `br_misrate`, `dtlb_rate` × 6 cores = 36 columns (float, clipped to physically meaningful bounds)
- PMIC inputs: `cpu_util_avg_pct`, `emc_util_pct`, `ram_used_mb`, `cpu_temp_c`, `tj_temp_c`, `cpu_gpu_cv_power_mw`, `vdd_in_power_mw` = 7 columns
- `label_freq_khz_p0`, `label_freq_khz_p4` — targets (next-step governor decision, shifted by 1)

**Total model inputs: 87. Targets: 2.**

### Benchmarks run by collect.sh (60s each, 10 total)

1. `stress-ng --cpu 4` — compute-bound, all cores
2. `stress-ng --stream 4` — memory bandwidth (STREAM benchmark)
3. NumPy 4096×4096 matmul — memory bandwidth (matrices exceed L3 cache)
4. NumPy random array access (256MB) — memory latency / pointer-chase
5. `stress-ng --branch 4` — branch predictor pressure
6. `stress-ng --icache 4` — instruction cache / frontend pressure
7. Bursty: 3s load / 3s idle × 10 — governor responsiveness
8. `stress-ng --cpu 1` — asymmetric, one core hot
9. `stress-ng --vm 2 --vm-bytes 1G` — VM pressure, page faults, TLB thrash
10. `make clean && make` — mixed real-world compilation

Total runtime: ~16 minutes.

### ML training notes

- **Class imbalance:** ~76% of labels are 1728 MHz (schedutil boosts to max under any heavy load). Use `class_weight` in `CrossEntropyLoss` during training — essential.
- **Model inputs at inference time:** All 87 inputs are reproducible in kernel. PMIC data is fed via a userspace daemon that parses tegrastats and writes to a `/dev` character device the kernel module reads.
- **Training:** Float32 in PyTorch. Quantize to fixed-point integers after training for kernel deployment.
- **Deployment:** Small MLP with weights exported as C arrays, forward pass implemented in kernel C.

---

## pmu_profiler (src/pmu_profiler)

Development/reference copy. Single-core sampler, used for early experiments.

**Tracked events:** INST_RETIRED, L1D_CACHE_REFILL, L1I_CACHE_REFILL, L2D_CACHE_REFILL, LL_CACHE_MISS_RD, BR_MIS_PRED

---

## Kernel Module Conventions

- All modules must be `MODULE_LICENSE("GPL")` to access GPL-only kernel symbols
- Use `pr_info(...)` / `pr_err(...)` for logging — userspace `printf` is unavailable
- Build artifacts (`*.ko`, `*.o`, `*.mod`, `*.cmd`, `Module.symvers`, `modules.order`) are gitignored
- `pmu_data.csv`, `pmic_data.csv`, `training_data.csv` are not gitignored — commit when new training data is collected

## INA3221 PMIC configuration

Persisted across reboots by `/etc/systemd/system/ina3221-config.service`:
```bash
/sys/class/hwmon/hwmon1/samples         # 4  (~4.4ms hardware averaging)
/sys/class/hwmon/hwmon1/update_interval # 1  (1ms poll)
```
Valid sample values: 1, 4, 16, 64, 128 (not 8 — silently clamped).
