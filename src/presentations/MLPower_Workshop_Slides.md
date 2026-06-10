# MLPower Workshop Slides — Content Outline

**Team:** Novel Alam · Daniel Hufnagle · William McGarry  
**Course:** CS 446  
**Theme (Google Slides):** Classic Light  
**Tone:** Group project — use *we/our/the team* throughout. Credit contributions by area, not by slide count.

---

## Slide 1 — Title

**Title:** MLPower: ML-Based CPU Frequency Scaling on Jetson Orin Nano

**Subtitle:** Replacing schedutil with workload-aware DVFS using PMU telemetry and power sensing

**Footer:** Novel Alam · Daniel Hufnagle · William McGarry · CS 446

**Visual (optional):** Board photo or simple block diagram of the full pipeline.

---

## Slide 2 — Talk in a Nutshell (Why Stay?)

**Goal for the audience:** Understand why Linux’s default governor is not enough for embedded power management, and how our end-to-end pipeline — from kernel sensing to ML inference — addresses it.

**Bullets:**
- Linux **schedutil** scales CPU frequency from scheduler utilization alone; it cannot tell *why* the CPU is busy.
- Memory-bound workloads often do not benefit from higher frequency, but schedutil still boosts — wasting power on the Jetson Orin Nano.
- **Our approach:** collect fine-grained PMU stall signals + real power draw, label optimal frequencies per workload regime, train a small MLP, and deploy it as a custom governor.
- **Key takeaway:** sensing (kernel), labeling (offline ML), training, and deployment are all required — no single piece stands alone.

**Speaker note:** This is the hook. Emphasize the *system* result, not one subsystem.

---

## Slide 3 — Problem & Motivation

**Bullets:**
- **DVFS** (Dynamic Voltage and Frequency Scaling): trade performance for power by changing CPU clock speed.
- Jetson Orin Nano has **6 cores** and **2 cpufreq policies** (policy0: cores 0–3, policy4: cores 4–5), with **22 discrete frequency steps** (115.2 MHz → 1728 MHz).
- **schedutil** boosts aggressively under load — in our training data, ~76% of governor labels sit at max frequency.
- We want a governor that distinguishes:
  - **Compute-bound** work (higher frequency helps)
  - **Memory-bound** work (stalling on DRAM — higher frequency mostly burns power)

**Visual (optional):** Two workload profiles side by side — high IPC vs high backend stall ratio.

---

## Slide 4 — High-Level Architecture

**Title:** How the Pieces Fit Together

**Diagram (recommended):**

```
Benchmarks / real workloads
        ↓
┌───────────────────────────────────────┐
│  Kernel sensing layer                 │
│  PMU profiler · PMIC driver · cpufreq   │
└───────────────────────────────────────┘
        ↓
  pmu_data.csv + pmic_data.csv
        ↓
  preprocess.py (join, features, labels)
        ↓
  training_data.csv → train.py (FreqMLP)
        ↓
  export_weights.py → governor deployment
        ↓
  compare_governors.sh (evaluation)
```

**Bullets:**
- **Sensing:** kernel modules expose hardware counters and power rails.
- **Dataset:** scripted two-phase collection across benchmarks and governors.
- **ML:** supervised classifier over 87 features (+ temporal deltas).
- **Deployment:** userspace RL governor today; in-kernel MLP governor planned.

---

## Slide 5 — Platform & Constraints

**Bullets:**
- **Hardware:** NVIDIA Jetson Orin Nano (aarch64, Cortex-A78).
- **Power modes:** `nvpmodel -m 2` (MAXN_SUPER) needed for full 1728 MHz range.
- **PMU limit:** only **6 hardware event counter slots** per core — event selection was a design decision.
- **Power rails:** INA3221 PMIC exposes CPU+GPU combined rail and system input power (no separate CPU-only rail).
- All kernel code is **out-of-tree** kbuild against `/lib/modules/$(uname -r)/build/`.

---

## Slide 6 — Data Collection Pipeline

**Title:** Building the Training Dataset (`collect.sh`)

**Bullets:**
- **Phase 1 — Frequency sweep:** run each benchmark at all 22 DVFS steps (userspace governor + `scaling_setspeed`) to learn per-workload power/performance curves.
- **Phase 2 — Governor dynamics:** replay benchmarks under schedutil, ondemand, conservative, performance, and powersave to capture realistic label behavior.
- **Benchmarks:** stress-ng (cpu, stream, branch, icache, vm), NumPy matmul / random access, bursty load, kernel compile, and others (~16 min full run).
- **Outputs:** `pmu_data.csv`, `pmic_data.csv`, `segments.csv` with monotonic timestamps for joining.
- PMIC also polled via `collect_pmic.py` (tegrastats) during early collection; kernel PMIC driver added for lower-latency access.

**Team note:** Collection orchestration and benchmark design were a shared effort across the group.

---

## Slide 7 — PMU: What It Is & Why We Need It

**Title:** ARM Performance Monitor Unit (PMU)

**Bullets:**
- The **PMU** is on-chip hardware in each CPU core that counts micro-architectural events (instructions, stalls, cache misses, branch mispredictions, TLB walks).
- **schedutil sees utilization %; the PMU sees mechanism** — e.g., cycles stalled waiting for data vs instructions actually retired.
- Normal userspace tools (`perf`, `tegrastats`) are too coarse or too slow for a ~10–16 ms governor loop on all 6 cores simultaneously.
- **Our PMU kernel module** (`src/data_collection/pmu_profiler.c`) programs ARM system registers and samples every core via `on_each_cpu()` IPI.

**Contribution credit (one line):** PMU profiler kernel module and `/dev/pmu_dc` character device — Novel Alam.

---

## Slide 8 — PMU Events & Features

**Bullets — 6 tracked events (all cores):**
| Slot | Event | Role |
|------|-------|------|
| 0 | INST_RETIRED | Baseline throughput |
| 1 | STALL_BACKEND | Cycles waiting on data — **primary scaling signal** |
| 2 | STALL_FRONTEND | Instruction fetch/decode stalls |
| 3 | LL_CACHE_MISS_RD | Demand reads reaching DRAM |
| 4 | BR_MIS_PRED | Branch predictor pressure |
| 5 | DTLB_WALK | Page-table walk pressure |
| + | PMCCNTR_EL0 | Cycle counter (per core) |

- **Derived ratios** (in `preprocess.py`): `stall_ratio`, `ipc`, `ll_miss_rate`, `br_misrate`, `dtlb_rate` per core.
- **Total model inputs from PMU path:** 42 raw deltas + 36 ratios + 2 current frequencies = 80 features before PMIC.

---

## Slide 9 — PMIC: What It Is & Why We Need It

**Title:** Power Measurement (INA3221 / PMIC)

**Bullets:**
- The **PMIC** on the board includes a Texas Instruments **INA3221** — a triple-channel current/voltage monitor on I2C.
- **Channel 1:** CPU+GPU+CV combined rail → `cpu_gpu_cv_power_mw`
- **Channel 3:** system input (VDD_IN) → `vdd_in_power_mw` — used as the **RL reward signal** for power-aware tuning
- Frequency decisions must be judged against **real watts**, not just performance counters.
- **Our PMIC kernel driver** (`src/pmic_driver/pmic_driver.c`) probes the INA3221, configures fast conversion times, and polls rails at ~1 ms.

**Contribution credit (one line):** PMIC I2C driver, power computation, and `/dev/pmic_driver_novel` — Novel Alam.

---

## Slide 10 — Kernel ↔ Userspace Bridge

**Title:** How Userspace Reads Live Hardware State

**Two columns:**

| PMU (`/dev/pmu_dc`) | PMIC (`/dev/pmic_driver_novel`) |
|---------------------|----------------------------------|
| `read()` returns `pmu_live_snapshot` struct | `read()` returns 14-byte INA3221 measurement |
| Timestamp, both policy frequencies, per-core event deltas | Shunt + bus voltages on 3 channels |
| Consumed by `rl_governor.py`, training scripts | `ioctl()` sets averaging / conversion time |
| Requires `insmod pmu_profiler.ko` | Requires `insmod pmic_driver.ko` |

**Also relevant:** `set_cpu_frequency.ko` exposes `/dev/set_cpu_frequency_novel` so userspace can **write** target frequencies (ioctl), complementing the read path.

**Key point:** Character devices with fixed struct layouts are the contract between kernel sensing and Python/C userspace.

---

## Slide 11 — Deep Dive: A Key Challenge (Pick One for Live Talk)

**Option A — Simultaneous all-core PMU sampling**
- Challenge: each core has private PMU state; timestamps must align across 6 cores.
- Approach: delayed workqueue (~16 ms) → `on_each_cpu(read_pmu_on_cpu)` → compute deltas vs previous window → cache under spinlock.
- Trade-off: only 6 event slots; choosing events meant dropping others (e.g., L1D refill).

**Option B — Labeling without an oracle governor**
- Challenge: no ground-truth “optimal frequency” exists in the kernel.
- Approach: Phase 1 sweep + K-means clustering on workload features → per-cluster efficiency-optimal frequency F*.
- Trade-off: labels reflect *measured efficiency*, not a proven global optimum.

**Recommendation:** Use **Option B** as the main deep-dive (ML-focused audience) or **Option A** (systems audience). Mention the other briefly.

---

## Slide 12 — Preprocessing & Labeling (`preprocess.py`)

**Bullets:**
- Join `pmu_data.csv` + `pmic_data.csv` on `timestamp_ns` (both use `CLOCK_MONOTONIC`).
- Engineer 87 features: frequencies, raw PMU deltas, derived ratios, PMIC telemetry.
- **K-means clustering** (MiniBatchKMeans / elbow method) groups Phase 1 rows into workload regimes.
- For each cluster, select **F\*** = frequency with best aggregate efficiency (throughput / power) on pure segments.
- Output: `training_data.csv` with `label_class` (0–21 DVFS index) and `segment_id` for leakage-safe train/val/test splits.

---

## Slide 13 — Model & Training (`train.py`)

**Bullets:**
- **Architecture:** `FreqMLP` — 174-dim input (current 87 features + delta from previous step) → 256 → 128 → 64 → 22-class softmax.
- **Task:** multiclass classification — predict next optimal DVFS step for policy0 (labels derived from sweep efficiency).
- **Class imbalance:** heavy skew toward max frequency — team uses **class weights** in `CrossEntropyLoss`.
- **Training:** segment-level split (70/15/15), early stopping, StandardScaler persisted for inference.
- **Deployment path:** `export_weights.py` / `export_weights_int8.py` → C headers for kernel inference.

---

## Slide 14 — Governor Deployment

**Bullets:**
- **Userspace RL governor** (`rl_governor.py`): reads `/dev/pmu_dc`, parses tegrastats for PMIC fields, runs MLP + PPO-style value network, writes frequency via cpufreq sysfs.
- **Kernel frequency control** (`set_cpu_frequency.ko`): validates DVFS table, calls `cpufreq_driver_target()` on all online CPUs.
- **Planned / in progress:** `mlp_governor.ko` — quantized MLP forward pass entirely in kernel for lower latency.
- Governor loop target: ~10–16 ms, aligned with PMU sampling window.

---

## Slide 15 — Evaluation (`compare_governors.sh`)

**Bullets:**
- Benchmarks ML governor against **schedutil, ondemand, performance, powersave**.
- **Primary metric: work-per-joule efficiency ratio** = (ML throughput/power) / (schedutil throughput/power).
  - > 1.0 → ML wins on efficiency
  - Avoids raw inst/mJ (frequency inflation on memory-bound stalls)
- Workloads: stress-ng cpu/stream, memory latency, bursty, matmul, compile, etc.
- Results logged to `governor/results/compare.csv`.

**Visual (optional):** Bar chart of efficiency ratio per benchmark from latest `compare.csv`.

---

## Slide 16 — Results & Observations (Team)

**Bullets (fill in with your actual numbers from `compare.csv`):**
- Memory-bound benchmarks (STREAM, random access): high `stall_ratio`, low IPC — opportunity to run below max freq.
- Compute-bound benchmarks (stress-ng --cpu): high IPC, low stalls — max frequency is appropriate.
- schedutil remains competitive on raw throughput because it always boosts; ML governor trades slight throughput for lower average power on selected workloads.
- **Honest framing:** results are workload-dependent; the pipeline is the main contribution even where ML does not beat schedutil on every benchmark.

---

## Slide 17 — Related Work

**Bullets:**
- **schedutil / ondemand / conservative:** kernel-native governors using utilization or load thresholds — no PMU stall awareness.
- **ML-DVFS literature:** often relies on coarse metrics (CPU util, OS counters) or simulators rather than embedded PMU + PMIC at ms resolution.
- **Jetson ecosystem:** `tegrastats`, `nvpmodel`, `jetson_clocks` — useful but not designed for closed-loop ML governors.
- **Our distinction:** reproducible 87-feature vector from real hardware, labeled by measured efficiency, deployed on a real board.

---

## Slide 18 — Conclusion & What’s Next

**Bullets:**
- Built an **end-to-end ML power management pipeline** on Jetson Orin Nano: sense → collect → label → train → deploy → evaluate.
- Demonstrated that **PMU stall ratios + PMIC power** carry signal schedutil cannot see.
- **Next steps (team):**
  - Finish in-kernel `mlp_governor.ko` deployment
  - RL fine-tuning with `vdd_in_power_mw` as reward
  - Expand benchmark suite and test on real applications
  - Quantized inference validation on aarch64

---

## Slide 19 — Q&A

**Title:** Questions?

**On-slide text:**
- MLPower — ML-Based Power Management for Jetson Orin Nano
- Team: Novel Alam · Daniel Hufnagle · William McGarry
- Repo: `[add GitHub URL]`

**Visual (optional):** QR code to repo or demo video.

---

# Backup Slides

## Backup A — Team Contributions (if asked “who did what?”)

| Area | Directory | Primary work |
|------|-----------|--------------|
| PMU kernel profiler | `src/data_collection/pmu_profiler.c` | ARM PMU init, all-core IPI sampling, `/dev/pmu_dc` |
| PMIC kernel driver | `src/pmic_driver/pmic_driver.c` | INA3221 I2C probe, power computation, `/dev/pmic_driver_novel` |
| Data collection | `src/data_collection/collect.sh`, `collect_pmic.py` | Two-phase sweep, benchmark orchestration |
| Preprocessing & labels | `src/data_collection/preprocess.py` | Feature engineering, K-means labeling |
| Model training | `src/training/` | FreqMLP, QAT, weight export |
| Governor & eval | `src/governor/` | RL governor, `compare_governors.sh` |
| CPU frequency control | `src/set_cpu_freq/` | `set_cpu_frequency.ko` char device |
| Early prototypes | `src/pmu_profiler/`, `src/cache_miss_counting/` | Initial PMU experiments |

*Adjust names if split differed — this reflects repo structure.*

---

## Backup B — Why Not perf or tegrastats Alone?

- **perf:** excellent for profiling sessions; not built for continuous all-core 10 ms sampling inside a governor loop.
- **tegrastats:** subprocess parsing, ~10 ms best case, no per-core PMU stall breakdown.
- **Kernel modules:** fixed struct layouts, `EXPORT_SYMBOL_GPL` for cross-module use, spinlock-cached snapshots.

---

## Backup C — DVFS Details (Jetson Orin Nano)

- policy0 → `cpufreq_get(0)`; policy4 → `cpufreq_get(4)`
- 22 discrete steps: 115200 … 1728000 kHz
- `nvpmodel -m 2` for full range
- Model outputs class index → mapped to kHz via `DVFS_STEPS` table

---

## Backup D — Build & Demo Commands

```bash
# PMU + PMIC modules
cd src/data_collection && make && sudo insmod pmu_profiler.ko
cd src/pmic_driver   && make && sudo insmod pmic_driver.ko
ls -l /dev/pmu_dc /dev/pmic_driver_novel

# Full dataset collection
cd src/data_collection && sudo bash ./collect.sh
python3 preprocess.py

# Train + evaluate
cd src/training && python3 train.py
cd src/governor  && sudo bash compare_governors.sh --quick
```

---

## Backup E — training_data.csv Schema (90 columns)

- Meta: `timestamp_ns`, `segment_id`
- Inputs: `freq_khz_p0/p4`, 42 PMU raw deltas, 36 derived ratios, 7 PMIC fields
- Label: `label_class` (DVFS index), `label_freq_khz_p0/p4`

---

# Presentation Tips

1. **Time split (~15–20 min talk):** ~2 min motivation, ~3 min architecture + platform, ~2 min data collection, ~3 min sensing (PMU + PMIC combined), ~3 min ML pipeline, ~2 min evaluation, ~1 min conclusion.
2. **Novel’s section:** Slides 7–10 are your technical contribution — aim for ~3–4 minutes total, not half the talk.
3. **Let teammates own:** slides 6, 12–16 during rehearsal if they built preprocess/train/governor/compare.
4. **Google Slides:** Create blank Classic Light deck → paste each slide’s title + bullets → add one architecture diagram on slide 4.
