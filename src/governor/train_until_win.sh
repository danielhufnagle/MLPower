#!/usr/bin/env bash
# train_until_win.sh — RL-train until the KERNEL governor beats schedutil generally.
#
# Each round:
#   1. RL-train for ROUND_SECS cycling through ALL workload types
#   2. Export weights → rebuild mlp_governor.ko
#   3. Kernel governor vs schedutil (--quick: 4 benchmarks, 20s each)
#   4. Print efficiency ratios; stop when ML wins ≥ 3/4
#
# Run as root from repo root:
#   sudo bash src/governor/train_until_win.sh

set -uo pipefail
cd "$(dirname "$0")"

export PYTHONPATH=/home/mlpower/.local/lib/python3.10/site-packages${PYTHONPATH:+:$PYTHONPATH}

TRAINING_DIR="../training"
MLP_GOV_DIR="../ML_GOVERNOR"
DC_DIR="../data_collection"
MODULE="$DC_DIR/pmu_profiler.ko"

BASE_MODEL="model_fp32.pt"
KL_COEF="0.02"
MAX_ROUNDS=100
# One full pass through all 14 workloads at 30s each = 420s.
# This ensures every regime gets gradient signal every round.
ROUND_SECS=420
SETTLE_SECS=8

GOV_PID=""
BENCH_PID=""

cleanup() {
    [ -n "$GOV_PID"   ] && kill "$GOV_PID"   2>/dev/null || true
    [ -n "$BENCH_PID" ] && kill "$BENCH_PID" 2>/dev/null || true
    pkill -f "stress-ng" 2>/dev/null || true
    for policy in /sys/devices/system/cpu/cpufreq/policy*/; do
        echo schedutil > "${policy}scaling_governor" 2>/dev/null || true
    done
    lsmod | grep -q ml_gov && rmmod ml_gov 2>/dev/null || true
    lsmod | grep -q pmu_profiler && rmmod pmu_profiler 2>/dev/null || true
}
trap cleanup EXIT

[ "$EUID" -ne 0 ] && { echo "error: must run as root"; exit 1; }

NVPMODEL_MODE=$(nvpmodel -q 2>/dev/null | grep "NV Power Mode" | awk '{print $NF}')
[ "$NVPMODEL_MODE" != "MAXN_SUPER" ] && { echo "error: sudo nvpmodel -m 2 first"; exit 1; }

lsmod | grep -q pmic_driver && rmmod pmic_driver 2>/dev/null || true

# Full workload suite — every regime the governor will face in production.
# Covers: compute, memory bandwidth, memory latency, branch prediction,
# instruction cache, VM pressure, bursty, mixed, ramped load, compilation.
# Pinned to cores 0-3 (policy0) so governor activity on cores 4-5 doesn't
# pollute the PMU readings or throughput numbers.
WORKLOADS=(
    "taskset -c 0-3 stress-ng --cpu 4 --timeout 30s --quiet"
    "taskset -c 0-3 stress-ng --cpu 4 --cpu-load 50 --timeout 30s --quiet"
    "taskset -c 0-3 stress-ng --cpu 1 --timeout 30s --quiet"
    "taskset -c 0-3 stress-ng --stream 4 --timeout 30s --quiet"
    "taskset -c 0-3 python3 -c \"
import numpy as np, time
a,b=np.random.rand(4096,4096),np.random.rand(4096,4096)
e=time.monotonic()+30
while time.monotonic()<e: np.dot(a,b)\""
    "taskset -c 0-3 python3 -c \"
import numpy as np, time
n=32*1024*1024; a=np.ones(n,dtype=np.float32); b=np.random.rand(n).astype(np.float32)
e=time.monotonic()+30
while time.monotonic()<e: np.multiply(b,2.0,out=a)\""
    "taskset -c 0-3 python3 -c \"
import numpy as np, time
arr=np.random.rand(32*1024*1024); e=time.monotonic()+30
while time.monotonic()<e: arr[np.random.randint(0,len(arr),50000)].sum()\""
    "taskset -c 0-3 stress-ng --branch 4 --timeout 30s --quiet"
    "taskset -c 0-3 stress-ng --icache 4 --timeout 30s --quiet"
    "taskset -c 0-3 stress-ng --vm 2 --vm-bytes 512M --timeout 30s --quiet"
    "bash -c 'for _ in 1 2 3; do taskset -c 0-3 stress-ng --cpu 4 --timeout 5s --quiet 2>/dev/null; sleep 5; done'"
    "bash -c 'taskset -c 0-3 stress-ng --cpu 4 --timeout 10s --quiet 2>/dev/null; taskset -c 0-3 stress-ng --stream 4 --timeout 10s --quiet 2>/dev/null; sleep 10'"
    "bash -c 'for load in 20 40 60 80 100; do taskset -c 0-3 stress-ng --cpu 4 --cpu-load \$load --timeout 6s --quiet 2>/dev/null; done'"
    "bash -c 'end=\$(( SECONDS+30 )); while [ \$SECONDS -lt \$end ]; do make -C ../data_collection clean>/dev/null 2>&1; make -C ../data_collection -j4>/dev/null 2>&1||true; done'"
)

pick_model() {
    local ckpt
    ckpt=$(ls -t "$TRAINING_DIR"/model_fp32_rl_*.pt 2>/dev/null | head -1) || true
    [ -n "$ckpt" ] && basename "$ckpt" || echo "$BASE_MODEL"
}

ensure_pmu_profiler() {
    lsmod | grep -q pmu_profiler && [ -c /dev/pmu_dc ] && return
    # Unload any stale/partial instance before reinserting
    rmmod pmu_profiler 2>/dev/null || true
    sleep 1
    insmod "$MODULE" || { echo "ERROR: insmod pmu_profiler failed"; dmesg | tail -5; exit 1; }
    sleep 1
    [ -c /dev/pmu_dc ] || { echo "ERROR: /dev/pmu_dc missing after insmod"; exit 1; }
}

avg_reward() {
    grep "ppo #" "$1" 2>/dev/null \
        | tail -20 \
        | grep -oP 'eff=\K[0-9.]+' \
        | awk '{s+=$1;n++} END {if(n) printf "%.4f (n=%d)",s/n,n; else print "n/a"}'
}

echo "===== train_until_win ====="
echo "base=$BASE_MODEL  kl=$KL_COEF  round=${ROUND_SECS}s  max_rounds=$MAX_ROUNDS"
echo "workloads: ${#WORKLOADS[@]} types, 30s each (full rotation per round)"
echo "test: kernel governor vs schedutil --quick (4 benchmarks, 20s each)"
echo "stop: ctrl+c when you see it plateau, or after $MAX_ROUNDS rounds"
echo ""

for round in $(seq 1 "$MAX_ROUNDS"); do
    current="$(pick_model)"
    echo "════════════════════════════════════════"
    echo "ROUND $round / $MAX_ROUNDS   model: $current"

    # ── phase 1: RL training ─────────────────────────────────────────────────
    ensure_pmu_profiler

    for policy in /sys/devices/system/cpu/cpufreq/policy*/; do
        echo userspace > "${policy}scaling_governor" 2>/dev/null || true
    done

    python3 -u rl_governor.py \
        --model        "$current" \
        --ref-model    "model_fp32.pt" \
        --kl-coef      "$KL_COEF" \
        --vf-coef      "0.1" \
        --ent-coef     "0.05" \
        --update-every 1000 \
        --save-every   500 \
        > /tmp/rl_gov.log 2>&1 &
    GOV_PID=$!
    sleep 3

    if ! kill -0 "$GOV_PID" 2>/dev/null; then
        echo "  ERROR: governor failed to start"
        cat /tmp/rl_gov.log
        exit 1
    fi

    # Cycle through all workloads, stopping after ROUND_SECS total.
    (
        end_t=$(( SECONDS + ROUND_SECS ))
        while [ $SECONDS -lt $end_t ]; do
            for wl in "${WORKLOADS[@]}"; do
                [ $SECONDS -lt $end_t ] || break
                eval "$wl" 2>/dev/null || true
            done
        done
    ) &
    BENCH_PID=$!

    echo "  training ${ROUND_SECS}s across ${#WORKLOADS[@]} workload types ..."
    sleep "$ROUND_SECS"

    kill -INT "$GOV_PID" 2>/dev/null || true
    sleep "$SETTLE_SECS"
    # Force kill if governor didn't exit gracefully after SETTLE_SECS
    kill -9 "$GOV_PID" 2>/dev/null || true
    kill "$BENCH_PID" 2>/dev/null || true
    pkill -f "stress-ng" 2>/dev/null || true
    wait "$GOV_PID"   2>/dev/null || true
    wait "$BENCH_PID" 2>/dev/null || true
    GOV_PID=""
    BENCH_PID=""

    ppo_count=$(grep -c "ppo #" /tmp/rl_gov.log 2>/dev/null || echo 0)
    echo "  PPO updates this round: $ppo_count"
    echo "  avg reward (last 20):   $(avg_reward /tmp/rl_gov.log)"
    echo "  log tail:"
    grep "ppo #" /tmp/rl_gov.log | tail -3 | sed 's/^/    /'

    # ── phase 2: export + kernel build ───────────────────────────────────────
    current="$(pick_model)"
    echo ""
    echo "  exporting $current → kernel ..."
    ( cd "$TRAINING_DIR" && python3 export_weights.py --model "$current" ) 2>/dev/null \
        || { echo "  ERROR: export failed"; continue; }
    make -C "$MLP_GOV_DIR" 2>/dev/null \
        || { echo "  ERROR: kernel build failed"; continue; }

    # ── phase 3: test kernel governor ────────────────────────────────────────
    echo "  testing kernel vs schedutil (--quick: 4 benchmarks × 20s) ..."

    bash compare_governors.sh --kernel --quick --vs-schedutil \
        --duration 20 2>&1 \
        | tee /tmp/cmp_out.txt \
        | grep -E "ratio|WINNER|wins [0-9]|cpu_all|stream|mem_latency|bursty" || true

    wins=$(grep -c "→ ml_governor" /tmp/cmp_out.txt 2>/dev/null || true)
    wins=${wins:-0}
    echo ""
    echo "  kernel ML wins: $wins / 4"

    echo ""
done

echo "Finished $MAX_ROUNDS rounds."
echo "Best model: $(pick_model)"
echo ""
echo "Run manually to see where it stands:"
echo "  sudo bash compare_governors.sh --kernel --quick --vs-schedutil --duration 20"
