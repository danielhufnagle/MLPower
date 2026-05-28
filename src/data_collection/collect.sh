#!/bin/bash
# Two-phase training data collection: frequency sweep (Phase 1) + governor dynamics (Phase 2).
# Usage: sudo bash ./collect.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE="$SCRIPT_DIR/pmu_profiler.ko"
POLLER="$SCRIPT_DIR/collect_pmic.py"

SWEEP_DURATION=20
GOV_DURATION=45
COOLDOWN=6
SETTLE=2

SWEEP_FREQS=(
    115200  192000  268800  345600  422400  499200
    576000  652800  729600  806400  883200  960000
    1036800 1113600 1190400 1267200 1344000 1420800
    1497600 1574400 1651200 1728000
)

GOVERNORS=(schedutil ondemand conservative performance powersave)

SEGMENTS_FILE="$SCRIPT_DIR/segments.csv"
SEGMENT_ID=0
POLLER_PID=""
WATCHDOG_PID=""

# ── helpers ───────────────────────────────────────────────────────────────────

log() { echo "[collect] $*"; }

stop_poller() {
    if [ -n "$POLLER_PID" ] && kill -0 "$POLLER_PID" 2>/dev/null; then
        log "stopping PMIC poller (pid $POLLER_PID)"
        kill -2 "$POLLER_PID"
        wait "$POLLER_PID" 2>/dev/null || true
    fi
}

# Rebinds INA3221 if it loses its I2C binding during the run.
ina3221_watchdog() {
    while true; do
        sleep 60
        if ! grep -ql "ina3221" /sys/class/hwmon/*/name 2>/dev/null; then
            log "WARNING: INA3221 not found in hwmon — rebinding driver"
            echo "1-0040" > /sys/bus/i2c/drivers/ina3221/bind 2>/dev/null || true
            sleep 1
            if grep -ql "ina3221" /sys/class/hwmon/*/name 2>/dev/null; then
                log "INA3221 rebound successfully"
            else
                log "ERROR: INA3221 rebind failed — PMIC data may be lost"
            fi
        fi
    done
}

cleanup() {
    log "cleaning up..."
    if [ -n "$WATCHDOG_PID" ] && kill -0 "$WATCHDOG_PID" 2>/dev/null; then
        kill "$WATCHDOG_PID" 2>/dev/null || true
    fi
    stop_poller
    set_governor schedutil 2>/dev/null || true
    if lsmod | grep -q pmu_profiler 2>/dev/null; then
        rmmod pmu_profiler 2>/dev/null || true
    fi
}

trap cleanup EXIT

set_freq() {
    local freq=$1
    for policy in /sys/devices/system/cpu/cpufreq/policy*/; do
        echo userspace > "${policy}scaling_governor"
        echo "$freq"   > "${policy}scaling_setspeed"
    done
}

set_governor() {
    local gov=$1
    for policy in /sys/devices/system/cpu/cpufreq/policy*/; do
        echo "$gov" > "${policy}scaling_governor"
    done
}

begin_segment() {
    local bench="$1" freq="${2:-0}" gov="$3"
    SEGMENT_ID=$(( SEGMENT_ID + 1 ))
    SEGMENT_START=$(date +%s%N)
    log "  [$SEGMENT_ID] bench=$bench freq=${freq}kHz gov=$gov"
}

end_segment() {
    local bench="$1" freq="${2:-0}" gov="$3"
    local end_ns
    end_ns=$(date +%s%N)
    echo "$SEGMENT_ID,$bench,$freq,$gov,$SEGMENT_START,$end_ns" >> "$SEGMENTS_FILE"
}

run_benchmark() {
    local name="$1" dur="$2"
    case "$name" in

        cpu_all)
            stress-ng --cpu 4 --timeout "${dur}s" --quiet ;;

        cpu_one)
            stress-ng --cpu 1 --timeout "${dur}s" --quiet ;;

        cpu_load_25)
            stress-ng --cpu 4 --cpu-load 25 --timeout "${dur}s" --quiet ;;

        cpu_load_50)
            stress-ng --cpu 4 --cpu-load 50 --timeout "${dur}s" --quiet ;;

        cpu_load_75)
            stress-ng --cpu 4 --cpu-load 75 --timeout "${dur}s" --quiet ;;

        crypto)
            timeout "$dur" bash -c \
                'while true; do openssl speed -elapsed aes-256-cbc sha256 2>/dev/null; done' \
                || true ;;

        stream)
            stress-ng --stream 4 --timeout "${dur}s" --quiet ;;

        matmul)
            # 4096x4096 float64 exceeds LLC — saturates DRAM bandwidth
            python3 -c "
import numpy as np, time
a = np.random.rand(4096, 4096)
b = np.random.rand(4096, 4096)
end = time.monotonic() + $dur
while time.monotonic() < end:
    np.dot(a, b)
" ;;

        mem_latency)
            # 256 MB random access — defeats prefetcher, stresses DRAM latency
            python3 -c "
import numpy as np, time
arr = np.random.rand(32 * 1024 * 1024)
end = time.monotonic() + $dur
while time.monotonic() < end:
    _ = arr[np.random.randint(0, len(arr), 50000)].sum()
" ;;

        mem_latency_large)
            # 2 GB random access — distinct from mem_latency, busts LLC completely
            python3 -c "
import numpy as np, time
arr = np.random.rand(256 * 1024 * 1024)
end = time.monotonic() + $dur
while time.monotonic() < end:
    _ = arr[np.random.randint(0, len(arr), 50000)].sum()
" ;;

        idle)
            sleep "$dur" ;;

        vm_pressure)
            stress-ng --vm 2 --vm-bytes 1G --timeout "${dur}s" --quiet ;;

        malloc)
            stress-ng --malloc 4 --timeout "${dur}s" --quiet ;;

        branch)
            stress-ng --branch 4 --timeout "${dur}s" --quiet ;;

        icache)
            stress-ng --icache 4 --timeout "${dur}s" --quiet ;;

        ml_inference)
            python3 -c "
import time
try:
    import torch, torch.nn as nn
    model = nn.Sequential(
        nn.Linear(256, 512), nn.ReLU(),
        nn.Linear(512, 512), nn.ReLU(),
        nn.Linear(512, 128), nn.ReLU(),
        nn.Linear(128, 10),
    )
    model.eval()
    x = torch.randn(64, 256)
    end = time.monotonic() + $dur
    with torch.no_grad():
        while time.monotonic() < end:
            model(x)
except ImportError:
    import numpy as np
    W = [np.random.randn(256,512), np.random.randn(512,512),
         np.random.randn(512,128), np.random.randn(128,10)]
    x = np.random.randn(64, 256)
    end = time.monotonic() + $dur
    while time.monotonic() < end:
        h = x
        for w in W:
            h = np.maximum(0, h @ w)
" ;;

        compile)
            make -C "$SCRIPT_DIR" clean 2>/dev/null || true
            timeout "$dur" make -C "$SCRIPT_DIR" 2>/dev/null || true ;;

        bursty)
            local cycles=$(( dur / 6 ))
            for _ in $(seq 1 "$cycles"); do
                stress-ng --cpu 4 --timeout 3s --quiet
                sleep 3
            done ;;

        multiphase)
            # compute → stream → idle, each for dur/3 seconds
            local phase=$(( dur / 3 ))
            stress-ng --cpu 4    --timeout "${phase}s" --quiet
            stress-ng --stream 4 --timeout "${phase}s" --quiet
            sleep "$phase" ;;

        ramp)
            # gradual load increase 10% → 100%
            local step=$(( dur / 7 ))
            for load in 10 25 40 55 70 85 100; do
                stress-ng --cpu 4 --cpu-load "$load" --timeout "${step}s" --quiet
            done ;;

        *)
            log "warning: unknown benchmark '$name', skipping" ;;
    esac
}

# ── preflight ─────────────────────────────────────────────────────────────────

if [ "$EUID" -ne 0 ]; then
    echo "error: must run as root (sudo bash ./collect.sh)"
    exit 1
fi

for cmd in stress-ng tegrastats openssl make; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "error: $cmd not found"
        exit 1
    fi
done

if ! python3 -c "import numpy" &>/dev/null; then
    echo "error: numpy not found — sudo apt install python3-numpy"
    exit 1
fi

if [ ! -f "$MODULE" ]; then
    echo "error: $MODULE not found — run 'make' in $SCRIPT_DIR first"
    exit 1
fi

if ! grep -q userspace \
        /sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors 2>/dev/null; then
    echo "error: userspace governor not available"
    exit 1
fi

NVPMODEL_MODE=$(nvpmodel -q 2>/dev/null | grep "NV Power Mode" | awk '{print $NF}')
if [ "$NVPMODEL_MODE" != "MAXN_SUPER" ]; then
    echo "error: nvpmodel must be MAXN_SUPER for full frequency range"
    echo "       current mode: ${NVPMODEL_MODE:-unknown}"
    echo "       run: sudo nvpmodel -m 2"
    exit 1
fi

# ── init ──────────────────────────────────────────────────────────────────────

if lsmod | grep -q pmic_driver; then
    log "unloading pmic_driver (conflicts with tegrastats INA3221 access)"
    rmmod pmic_driver
fi

if lsmod | grep -q pmu_profiler; then
    rmmod pmu_profiler
fi

log "loading pmu_profiler"
insmod "$MODULE"
dmesg | tail -3

echo "segment_id,benchmark,freq_khz,governor,start_ns,end_ns" > "$SEGMENTS_FILE"

log "starting INA3221 watchdog"
ina3221_watchdog &
WATCHDOG_PID=$!

log "starting PMIC poller"
python3 "$POLLER" &
POLLER_PID=$!
sleep 2

# Verify PMIC data is flowing before committing to a 4.5 hour run.
PMIC_ROWS_EARLY=$(( $(wc -l < "$SCRIPT_DIR/pmic_data.csv" 2>/dev/null) - 1 ))
if [ "$PMIC_ROWS_EARLY" -lt 10 ]; then
    log "ERROR: PMIC smoke test failed — only $PMIC_ROWS_EARLY rows after 2s"
    log "       Expected ~200 rows. INA3221 may not be producing power data."
    log "       Check: grep -r ina3221 /sys/class/hwmon/*/name"
    log "       Fix:   echo 1-0040 | sudo tee /sys/bus/i2c/drivers/ina3221/bind"
    exit 1
fi
log "PMIC smoke test passed — $PMIC_ROWS_EARLY rows in first 2s"

# ── phase 1: frequency sweep ──────────────────────────────────────────────────

SWEEP_BENCHMARKS=(
    cpu_all cpu_one cpu_load_25 cpu_load_50 cpu_load_75
    stream matmul
    mem_latency mem_latency_large
    branch icache
    vm_pressure malloc
    ml_inference crypto
    idle
    bursty multiphase ramp
)

TOTAL_SWEEP=$(( ${#SWEEP_BENCHMARKS[@]} * ${#SWEEP_FREQS[@]} ))
log "=== PHASE 1: frequency sweep — ${#SWEEP_BENCHMARKS[@]} benchmarks × ${#SWEEP_FREQS[@]} freqs = $TOTAL_SWEEP segments ==="

SEG_NUM=0
for bench in "${SWEEP_BENCHMARKS[@]}"; do
    for freq in "${SWEEP_FREQS[@]}"; do
        SEG_NUM=$(( SEG_NUM + 1 ))
        log "sweep $SEG_NUM/$TOTAL_SWEEP: $bench @ ${freq} kHz"
        set_freq "$freq"
        sleep "$SETTLE"
        begin_segment "$bench" "$freq" "userspace"
        run_benchmark "$bench" "$SWEEP_DURATION"
        end_segment "$bench" "$freq" "userspace"
        sleep "$COOLDOWN"
    done
done

set_governor schedutil
sleep 2

# ── phase 2: governor dynamics ────────────────────────────────────────────────

GOV_BENCHMARKS=(
    cpu_all cpu_load_50
    stream matmul mem_latency
    branch vm_pressure
    ml_inference compile
    bursty multiphase ramp
)

TOTAL_GOV=$(( ${#GOVERNORS[@]} * ${#GOV_BENCHMARKS[@]} ))
log "=== PHASE 2: governor dynamics — ${#GOVERNORS[@]} governors × ${#GOV_BENCHMARKS[@]} benchmarks = $TOTAL_GOV segments ==="

SEG_NUM=0
for gov in "${GOVERNORS[@]}"; do
    log "--- governor: $gov ---"
    set_governor "$gov"
    sleep 1
    for bench in "${GOV_BENCHMARKS[@]}"; do
        SEG_NUM=$(( SEG_NUM + 1 ))
        log "gov $SEG_NUM/$TOTAL_GOV: $gov / $bench"
        begin_segment "$bench" "" "$gov"
        run_benchmark "$bench" "$GOV_DURATION"
        end_segment "$bench" "" "$gov"
        sleep "$COOLDOWN"
    done
done

set_governor schedutil

# ── summary ───────────────────────────────────────────────────────────────────

stop_poller

PMU_ROWS=$(( $(wc -l < "$SCRIPT_DIR/pmu_data.csv") - 1 ))
PMIC_ROWS=$(( $(wc -l < "$SCRIPT_DIR/pmic_data.csv") - 1 ))
SEG_COUNT=$(( $(wc -l < "$SEGMENTS_FILE") - 1 ))

log "=== collection complete ==="
log "  segments : $SEG_COUNT"
log "  pmu rows : $PMU_ROWS"
log "  pmic rows: $PMIC_ROWS"
log "  files    : pmu_data.csv  pmic_data.csv  segments.csv"
log ""
log "Next: python3 preprocess.py"
