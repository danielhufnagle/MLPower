#!/bin/bash
# collect.sh - Orchestrate PMU + PMIC data collection across benchmark workloads.
#
# Usage: sudo bash ./collect.sh
#
# Loads pmu_profiler kernel module, starts PMIC poller, runs benchmarks,
# then stops everything cleanly. Produces two CSVs:
#   pmu_data.csv   - PMU event deltas every ~16ms
#   pmic_data.csv  - PMIC power + CPU frequency every ~10ms
#
# If a previous run exists, it is backed up to pmu_data_run<N>.csv before
# being overwritten, so all runs are preserved for combined training.
#
# Workload coverage (10 benchmarks):
#   1.  Compute-bound        — all cores pegged, high IPC, low stall
#   2.  Memory bandwidth     — STREAM benchmark, saturates DRAM bus
#   3.  Memory bandwidth     — numpy 4096x4096 matmul (too large for cache)
#   4.  Memory latency       — random array access, pointer-chase pattern
#   5.  Branch-heavy         — hammers branch predictor
#   6.  Frontend-bound       — instruction cache pressure
#   7.  Bursty               — alternating 3s load / 3s idle x10
#   8.  Asymmetric           — one core hot, five idle
#   9.  VM pressure          — page faults + TLB thrash
#  10.  Mixed real-world     — compile kernel module

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE="$SCRIPT_DIR/pmu_profiler.ko"
POLLER="$SCRIPT_DIR/collect_pmic.py"
BENCHMARK_DURATION=60
IDLE_DURATION=30
COOLDOWN=15

# ── helpers ───────────────────────────────────────────────────────────────────

log() { echo "[collect] $*"; }

stop_poller() {
    if [ -n "$POLLER_PID" ] && kill -0 "$POLLER_PID" 2>/dev/null; then
        log "stopping PMIC poller (pid $POLLER_PID)"
        kill -2 "$POLLER_PID"
        wait "$POLLER_PID" 2>/dev/null || true
    fi
}

cleanup() {
    log "cleaning up..."
    stop_poller
    if [ "$EUID" -eq 0 ] && lsmod | grep -q pmu_profiler; then
        log "unloading pmu_profiler"
        rmmod pmu_profiler
    fi
}

trap cleanup EXIT

# ── preflight checks ──────────────────────────────────────────────────────────

if [ "$EUID" -ne 0 ]; then
    echo "error: must run as root (sudo bash ./collect.sh)"
    exit 1
fi

if [ ! -f "$MODULE" ]; then
    echo "error: $MODULE not found — run 'make' in $SCRIPT_DIR first"
    exit 1
fi

if [ ! -f "$POLLER" ]; then
    echo "error: $POLLER not found"
    exit 1
fi

if ! command -v stress-ng &>/dev/null; then
    echo "error: stress-ng not found — sudo apt install stress-ng"
    exit 1
fi

if ! python3 -c "import numpy" &>/dev/null; then
    echo "error: numpy not found — sudo apt install python3-numpy"
    exit 1
fi

if ! command -v tegrastats &>/dev/null; then
    echo "error: tegrastats not found"
    exit 1
fi

# ── load module ───────────────────────────────────────────────────────────────

if lsmod | grep -q pmu_profiler; then
    log "unloading existing pmu_profiler"
    rmmod pmu_profiler
fi

log "loading pmu_profiler"
insmod "$MODULE"
dmesg | tail -5

# ── start PMIC poller ─────────────────────────────────────────────────────────

log "starting PMIC poller"
python3 "$POLLER" &
POLLER_PID=$!
log "PMIC poller pid: $POLLER_PID"
sleep 0.5

# ── benchmarks ────────────────────────────────────────────────────────────────

log "=== idle phase: ${IDLE_DURATION}s ==="
sleep "$IDLE_DURATION"

# 1. Compute-bound (all cores) ─────────────────────────────────────────────────
log "=== benchmark 1/10: compute-bound all cores (stress-ng --cpu 4) ==="
stress-ng --cpu 4 --timeout "${BENCHMARK_DURATION}s" --metrics-brief
sleep "$COOLDOWN"

# 2. Memory bandwidth (STREAM) ─────────────────────────────────────────────────
log "=== benchmark 2/10: memory bandwidth (stress-ng --stream 4) ==="
stress-ng --stream 4 --timeout "${BENCHMARK_DURATION}s" --metrics-brief
sleep "$COOLDOWN"

# 3. Memory bandwidth (large matmul — too big for cache) ───────────────────────
log "=== benchmark 3/10: memory bandwidth (numpy 4096x4096 matmul) ==="
python3 - <<'EOF'
import numpy as np, time
# 4096x4096 float64 = 128MB per matrix — far exceeds L3 cache
a = np.random.rand(4096, 4096)
b = np.random.rand(4096, 4096)
end = time.monotonic() + 60
while time.monotonic() < end:
    np.dot(a, b)
EOF
sleep "$COOLDOWN"

# 4. Memory latency (random access — pointer-chase pattern) ────────────────────
log "=== benchmark 4/10: memory latency (random array access) ==="
python3 - <<'EOF'
import numpy as np, time
# 256MB array — kills L1/L2/L3 cache, forces every access to DRAM
arr = np.random.rand(32 * 1024 * 1024)
end = time.monotonic() + 60
while time.monotonic() < end:
    idx = np.random.randint(0, len(arr), 50000)
    _ = arr[idx].sum()
EOF
sleep "$COOLDOWN"

# 5. Branch-heavy ──────────────────────────────────────────────────────────────
log "=== benchmark 5/10: branch-heavy (stress-ng --branch 4) ==="
stress-ng --branch 4 --timeout "${BENCHMARK_DURATION}s" --metrics-brief
sleep "$COOLDOWN"

# 6. Frontend-bound (instruction cache pressure) ───────────────────────────────
log "=== benchmark 6/10: frontend-bound (stress-ng --icache 4) ==="
stress-ng --icache 4 --timeout "${BENCHMARK_DURATION}s" --metrics-brief
sleep "$COOLDOWN"

# 7. Bursty (alternating 3s load / 3s idle) ────────────────────────────────────
log "=== benchmark 7/10: bursty (3s on / 3s off x10) ==="
for i in $(seq 1 10); do
    stress-ng --cpu 4 --timeout 3s --quiet
    sleep 3
done
sleep "$COOLDOWN"

# 8. Asymmetric (one core hot, five idle) ──────────────────────────────────────
log "=== benchmark 8/10: asymmetric (stress-ng --cpu 1) ==="
stress-ng --cpu 1 --timeout "${BENCHMARK_DURATION}s" --metrics-brief
sleep "$COOLDOWN"

# 9. VM pressure (page faults + TLB thrash) ────────────────────────────────────
log "=== benchmark 9/10: VM pressure (stress-ng --vm 2 --vm-bytes 1G) ==="
stress-ng --vm 2 --vm-bytes 1G --timeout "${BENCHMARK_DURATION}s" --metrics-brief
sleep "$COOLDOWN"

# 10. Mixed real-world (compile kernel module) ─────────────────────────────────
log "=== benchmark 10/10: mixed real-world (compile kernel module) ==="
make -C "$SCRIPT_DIR" clean
make -C "$SCRIPT_DIR"

log "=== idle phase: ${IDLE_DURATION}s ==="
sleep "$IDLE_DURATION"

# ── stop everything ───────────────────────────────────────────────────────────

log "all benchmarks complete"
stop_poller

log "unloading pmu_profiler"
rmmod pmu_profiler

dmesg | tail -10

log "done — CSVs written to $SCRIPT_DIR"
log "  pmu_data.csv"
log "  pmic_data.csv"
