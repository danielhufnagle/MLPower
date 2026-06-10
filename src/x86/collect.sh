#!/bin/bash
# Two-phase training data collection for x86 (AMD).
# Usage: sudo bash ./collect.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE="$SCRIPT_DIR/pmu_profiler.ko"
POLLER_PY="$SCRIPT_DIR/collect_power.py"
PYTHON_VENV="$SCRIPT_DIR/venv/bin/python3"

SWEEP_DURATION=2
GOV_DURATION=2
COOLDOWN=1
SETTLE=1

# AMD 7640U range approx 400MHz to 4.9GHz
SWEEP_FREQS=(
    400000 2000000 4800000
)

GOVERNORS=(performance powersave)

SEGMENTS_FILE="$SCRIPT_DIR/segments.csv"
SEGMENT_ID=0
POLLER_PID=""

# ── helpers ───────────────────────────────────────────────────────────────────

log() { echo "[collect] $*"; }

stop_poller() {
    if [ -n "$POLLER_PID" ] && kill -0 "$POLLER_PID" 2>/dev/null; then
        log "stopping power poller (pid $POLLER_PID)"
        kill -2 "$POLLER_PID"
        wait "$POLLER_PID" 2>/dev/null || true
    fi
}

cleanup() {
    log "cleaning up..."
    stop_poller
    if lsmod | grep -q pmu_profiler 2>/dev/null; then
        rmmod pmu_profiler 2>/dev/null || true
    fi
    # Restore amd-pstate status if changed
    echo active | sudo tee /sys/devices/system/cpu/amd_pstate/status 2>/dev/null || true
}

trap cleanup EXIT

set_freq() {
    local freq=$1
    # Ensure passive mode for frequency setting
    echo passive | sudo tee /sys/devices/system/cpu/amd_pstate/status >/dev/null
    for policy in /sys/devices/system/cpu/cpufreq/policy*/; do
        echo "$freq" > "${policy}scaling_min_freq"
        echo "$freq" > "${policy}scaling_max_freq"
    done
}

set_governor() {
    local gov=$1
    # Switch back to active for EPP governors if needed, or stay in passive
    echo active | sudo tee /sys/devices/system/cpu/amd_pstate/status >/dev/null
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
            stress-ng --cpu 12 --timeout "${dur}s" --quiet ;;
        cpu_one)
            stress-ng --cpu 1 --timeout "${dur}s" --quiet ;;
        cpu_load_50)
            stress-ng --cpu 12 --cpu-load 50 --timeout "${dur}s" --quiet ;;
        crypto)
            timeout "$dur" bash -c 'while true; do openssl speed -elapsed aes-256-cbc sha256 2>/dev/null; done' || true ;;
        stream)
            stress-ng --stream 12 --timeout "${dur}s" --quiet ;;
        matmul)
            "$PYTHON_VENV" -c "import numpy as np, time; a = np.random.rand(4096, 4096); b = np.random.rand(4096, 4096); end = time.monotonic() + $dur; 
while time.monotonic() < end: np.dot(a, b)" ;;
        mem_latency)
            "$PYTHON_VENV" -c "import numpy as np, time; arr = np.random.rand(32 * 1024 * 1024); end = time.monotonic() + $dur; 
while time.monotonic() < end: _ = arr[np.random.randint(0, len(arr), 50000)].sum()" ;;
        idle)
            sleep "$dur" ;;
        *)
            log "warning: unknown benchmark '$name', skipping" ;;
    esac
}

# ── preflight ─────────────────────────────────────────────────────────────────

if [ "$EUID" -ne 0 ]; then
    echo "error: must run as root (sudo bash ./collect.sh)"
    exit 1
fi

if [ ! -f "$MODULE" ]; then
    log "building module..."
    make -C "$SCRIPT_DIR"
fi

# ── init ──────────────────────────────────────────────────────────────────────

if lsmod | grep -q pmu_profiler; then
    rmmod pmu_profiler
fi

log "loading pmu_profiler"
insmod "$MODULE"

echo "segment_id,benchmark,freq_khz,governor,start_ns,end_ns" > "$SEGMENTS_FILE"

log "starting power poller"
"$PYTHON_VENV" "$POLLER_PY" &
POLLER_PID=$!
sleep 2

# ── phase 1: frequency sweep ──────────────────────────────────────────────────

SWEEP_BENCHMARKS=(cpu_all cpu_one stream matmul mem_latency crypto idle)

TOTAL_SWEEP=$(( ${#SWEEP_BENCHMARKS[@]} * ${#SWEEP_FREQS[@]} ))
log "=== PHASE 1: frequency sweep ==="

for bench in "${SWEEP_BENCHMARKS[@]}"; do
    for freq in "${SWEEP_FREQS[@]}"; do
        log "sweep: $bench @ ${freq} kHz"
        set_freq "$freq"
        sleep "$SETTLE"
        begin_segment "$bench" "$freq" "passive"
        run_benchmark "$bench" "$SWEEP_DURATION"
        end_segment "$bench" "$freq" "passive"
        sleep "$COOLDOWN"
    done
done

# ── phase 2: governor dynamics ────────────────────────────────────────────────

log "=== PHASE 2: governor dynamics ==="

for gov in "${GOVERNORS[@]}"; do
    set_governor "$gov"
    for bench in "${SWEEP_BENCHMARKS[@]}"; do
        log "gov: $gov / $bench"
        begin_segment "$bench" "" "$gov"
        run_benchmark "$bench" "$GOV_DURATION"
        end_segment "$bench" "" "$gov"
        sleep "$COOLDOWN"
    done
done

log "=== collection complete ==="
