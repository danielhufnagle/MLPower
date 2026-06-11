#!/bin/bash
# compare_governors.sh — Benchmark ML governor vs Linux governors.
#
# METRIC: work-per-joule efficiency ratio vs schedutil.
#
#   efficiency       = throughput / avg_power_mW
#   efficiency_ratio = ML_efficiency / schedutil_efficiency
#
#   > 1.0  → ML does more useful work per joule  (ML wins)
#   = 1.0  → tied
#   < 1.0  → ML is less efficient                (ML loses)
#
# WHY NOT inst/mJ:
#   Raw instruction counts inflate with CPU frequency even on memory-bound
#   workloads — the CPU just spins faster in stall loops doing no extra work.
#   schedutil wins inst/mJ almost by definition because it always runs at
#   max frequency.
#
# WHY throughput/power:
#   Throughput is benchmark-native: bogo-ops/s (stress-ng), iterations/s
#   (Python workloads). It measures actual completed work — memory bandwidth
#   achieved, matrices computed, random accesses done. A governor that runs
#   at lower frequency and achieves the same bandwidth at less power wins.
#   One that sacrifices throughput to save power is penalized proportionally.
#
# Usage:
#   sudo bash compare_governors.sh
#   sudo bash compare_governors.sh --quick          # 4 benchmarks
#   sudo bash compare_governors.sh --vs-schedutil   # only vs schedutil
#   sudo bash compare_governors.sh --model <file>   # specific ML model
#   sudo bash compare_governors.sh --duration <s>   # seconds per run (default 30)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DC_DIR="$SCRIPT_DIR/../data_collection"
TRAINING_DIR="$SCRIPT_DIR/../training"
MODULE="$DC_DIR/pmu_profiler.ko"
GOVERNOR_PY="$SCRIPT_DIR/rl_governor.py"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"
CSV="$RESULTS_DIR/compare.csv"

DURATION=30
SETTLE=3
COOLDOWN=20

LINUX_GOVERNORS=(schedutil ondemand performance powersave)

# Full benchmark set; --quick uses the first 4; --fast uses 2
ALL_BENCHMARKS=(cpu_all stream mem_latency membw bursty matmul branch icache compile)
QUICK_BENCHMARKS=(cpu_all stream mem_latency inference compile bursty)
FAST_BENCHMARKS=(cpu_all stream)

QUICK=0
FAST=0
VS_SCHEDUTIL=0
ML_MODEL=""
KERNEL_GOV=0
args=("$@")
for ((i=0; i<${#args[@]}; i++)); do
    [[ "${args[$i]}" == "--quick"        ]] && QUICK=1
    [[ "${args[$i]}" == "--fast"         ]] && FAST=1
    [[ "${args[$i]}" == "--vs-schedutil" ]] && VS_SCHEDUTIL=1
    [[ "${args[$i]}" == "--kernel"       ]] && KERNEL_GOV=1
    [[ "${args[$i]}" == "--model"        ]] && ML_MODEL="${args[$((i+1))]}"
    [[ "${args[$i]}" == "--duration"     ]] && DURATION="${args[$((i+1))]}"
done

# --fast: short durations, 2 benchmarks, vs schedutil only — runs in ~90 seconds
if [ "$FAST" -eq 1 ]; then
    DURATION=15
    SETTLE=2
    COOLDOWN=2
    VS_SCHEDUTIL=1
fi

if [ -z "$ML_MODEL" ]; then
    ML_MODEL="model_fp32.pt"
fi
if [ "$KERNEL_GOV" -eq 1 ]; then
    echo "[compare] ML mode:  kernel governor (mlp_governor.ko)"
else
    echo "[compare] ML model: $ML_MODEL"
fi
echo "[compare] metric:   throughput/power  (work-per-joule, ratio vs schedutil)"

if   [ "$FAST"  -eq 1 ]; then BENCHMARKS=("${FAST_BENCHMARKS[@]}")
elif [ "$QUICK" -eq 1 ]; then BENCHMARKS=("${QUICK_BENCHMARKS[@]}")
else                           BENCHMARKS=("${ALL_BENCHMARKS[@]}")
fi
[ "$VS_SCHEDUTIL" -eq 1 ] && LINUX_GOVERNORS=(schedutil)

ML_GOV_PID=""
TEGRA_PID=""
BENCH_PID=""
# Remember whether modules were already loaded before we started,
# so cleanup only removes them if WE loaded them.
MODULE_WAS_LOADED=0
KERNEL_MOD_WAS_LOADED=0

lsmod | grep -q pmu_profiler  && MODULE_WAS_LOADED=1
lsmod | grep -q ml_gov        && KERNEL_MOD_WAS_LOADED=1

MLP_GOV_KO="$(dirname "$SCRIPT_DIR")/ML_GOVERNOR/ml_gov.ko"

log() { echo "[compare] $*"; }

cleanup() {
    [ -n "$BENCH_PID"  ] && kill "$BENCH_PID"  2>/dev/null || true
    [ -n "$TEGRA_PID"  ] && kill "$TEGRA_PID"  2>/dev/null || true
    if [ -n "$ML_GOV_PID" ]; then
        kill "$ML_GOV_PID" 2>/dev/null || true
        for _w in 1 2 3 4 5; do
            kill -0 "$ML_GOV_PID" 2>/dev/null || break
            sleep 1
        done
        ML_GOV_PID=""
    fi
    # Switch away from mlp BEFORE unloading mlp_governor (module refuses rmmod if in use).
    for policy in /sys/devices/system/cpu/cpufreq/policy*/; do
        echo schedutil > "${policy}scaling_governor" 2>/dev/null || true
    done
    sleep 1
    
    if [ "$KERNEL_MOD_WAS_LOADED" -eq 0 ]; then
        lsmod | grep -q ml_gov && rmmod ml_gov 2>/dev/null || true
    fi

    # Only unload pmu_profiler if we loaded it.
    if [ "$MODULE_WAS_LOADED" -eq 0 ]; then
        for _attempt in 1 2 3; do
            lsmod | grep -q pmu_profiler || break
            rmmod pmu_profiler 2>/dev/null && break
            sleep 2
        done
    fi

    # Restore INA3221 to kernel driver if we took it.
    if [ "${PMIC_MOD_WAS_LOADED:-0}" -eq 0 ]; then
        lsmod | grep -q pmic_driver && rmmod pmic_driver 2>/dev/null || true
        echo "1-0040" > /sys/bus/i2c/drivers/ina3221/bind 2>/dev/null || true
    fi
}
trap cleanup EXIT

[ "$EUID" -ne 0 ] && { echo "error: must run as root"; exit 1; }

export PYTHONPATH=/home/mlpower/.local/lib/python3.10/site-packages${PYTHONPATH:+:$PYTHONPATH}


PMIC_MOD_WAS_LOADED=0
lsmod | grep -q pmic_driver && PMIC_MOD_WAS_LOADED=1

if [ "$PMIC_MOD_WAS_LOADED" -eq 0 ]; then
    # Release the device from the kernel ina3221 driver so our probe fires.
    echo "1-0040" > /sys/bus/i2c/drivers/ina3221/unbind 2>/dev/null || true
    sleep 0.2
    insmod "$(dirname "$SCRIPT_DIR")/pmic_driver/pmic_driver.ko" || {
        log "ERROR: pmic_driver failed to load — rebinding to kernel ina3221"
        echo "1-0040" > /sys/bus/i2c/drivers/ina3221/bind 2>/dev/null || true
        exit 1
    }
    log "pmic_driver loaded, INA3221 bound to custom driver"
fi

NVPMODEL_MODE=$(nvpmodel -q 2>/dev/null | grep "NV Power Mode" | awk '{print $NF}')
if [ "$NVPMODEL_MODE" != "MAXN_SUPER" ]; then
    echo "error: run 'sudo nvpmodel -m 2' (current: ${NVPMODEL_MODE:-unknown})"; exit 1
fi

if [ "$KERNEL_GOV" -eq 1 ]; then
    # Ensure PMU profiler is loaded
    insmod "$MODULE" 2>/dev/null || true
    
    if [ ! -f "$MLP_GOV_KO" ]; then
        log "ERROR: $MLP_GOV_KO not found"
        exit 1
    fi
    lsmod | grep -q ml_gov && rmmod ml_gov 2>/dev/null || true
    insmod "$MLP_GOV_KO" || { log "ERROR: insmod ml_gov failed"; exit 1; }
    sleep 1
    lsmod | grep -q ml_gov || { log "ERROR: ml_gov not loaded"; exit 1; }
    log "ml_gov.ko loaded"
else
    # Userspace governor reads /dev/pmu_dc — needs pmu_profiler.
    for _attempt in 1 2 3; do
        lsmod | grep -q pmu_profiler || break
        rmmod pmu_profiler 2>/dev/null && break
        log "pmu_profiler busy, waiting..."
        sleep 2
    done
    insmod "$MODULE" 2>/dev/null || { sleep 2; insmod "$MODULE"; }
    sleep 1
    lsmod | grep -q pmu_profiler || { log "ERROR: pmu_profiler failed to load"; exit 1; }
    log "pmu_profiler loaded"
fi

# ── governor helpers ──────────────────────────────────────────────────────────

set_linux_governor() {
    local gov=$1
    for policy in /sys/devices/system/cpu/cpufreq/policy*/; do
        echo "$gov" > "${policy}scaling_governor"
    done
}

start_ml_governor() {
    if [ "$KERNEL_GOV" -eq 1 ]; then
        for policy in /sys/devices/system/cpu/cpufreq/policy*/; do
            echo userspace > "${policy}scaling_governor" 2>/dev/null || true
        done
        log "kernel ml_gov active (userspace policy set)"
        return
    fi
    kill "$ML_GOV_PID" 2>/dev/null || true
    python3 -u "$GOVERNOR_PY" --inference-only --log-gates --model "$ML_MODEL" \
        > "$RESULTS_DIR/ml_gov.log" 2>&1 &
    ML_GOV_PID=$!
    sleep 2
    if ! kill -0 "$ML_GOV_PID" 2>/dev/null; then
        log "ERROR: ML governor failed — see $RESULTS_DIR/ml_gov.log"
        cat "$RESULTS_DIR/ml_gov.log"
        exit 1
    fi
}

stop_ml_governor() {
    if [ "$KERNEL_GOV" -eq 1 ]; then
        # Switch off mlp before the next governor takes over.
        for policy in /sys/devices/system/cpu/cpufreq/policy*/; do
            echo schedutil > "${policy}scaling_governor" 2>/dev/null || true
        done
        sleep 1
        return
    fi
    [ -n "$ML_GOV_PID" ] && kill "$ML_GOV_PID" 2>/dev/null || true
    ML_GOV_PID=""
}

# ── benchmark runners ─────────────────────────────────────────────────────────
# stress-ng: remove --quiet, add --metrics-brief so we can parse bogo-ops/s
# Python:    print throughput (per second) to stdout at end

start_bench() {
    local name=$1 dur=$2 bench_out=$3
    # Pin benchmarks to cores 0-3 (policy0). The governor runs on cores 4-5
    # (policy4, capped at 576 MHz). Without pinning, benchmark threads can land
    # on policy4 cores and run 3× slower, making the policy4 cap look like a
    # throughput regression rather than a power saving.
    local TS="taskset -c 0-3"
    case "$name" in
        cpu_all)
            $TS stress-ng --cpu 4 --timeout "${dur}s" --metrics-brief 2>"$bench_out" &
            BENCH_PID=$! ;;
        single_core)
            $TS stress-ng --cpu 1 --timeout "${dur}s" --metrics-brief 2>"$bench_out" &
            BENCH_PID=$! ;;
        stream)
            $TS stress-ng --stream 4 --timeout "${dur}s" --metrics-brief 2>"$bench_out" &
            BENCH_PID=$! ;;
        branch)
            $TS stress-ng --branch 4 --timeout "${dur}s" --metrics-brief 2>"$bench_out" &
            BENCH_PID=$! ;;
        icache)
            $TS stress-ng --icache 4 --timeout "${dur}s" --metrics-brief 2>"$bench_out" &
            BENCH_PID=$! ;;
        bursty)
            # 5 × (3s active + 3s idle). Each stress-ng run appends its metrics.
            # parse_throughput sums total bogo-ops / full duration (including idle).
            # A governor that drops frequency faster during idle saves power without
            # losing bogo-ops during active phases → higher work-per-joule.
            ( for _ in $(seq 1 5); do
                  $TS stress-ng --cpu 4 --timeout 3s --metrics-brief 2>>"$bench_out"
                  sleep 3
              done ) &
            BENCH_PID=$! ;;
        matmul)
            $TS python3 -c "
import numpy as np, time
a, b = np.random.rand(4096,4096), np.random.rand(4096,4096)
end = time.monotonic() + ${dur}
iters = 0
while time.monotonic() < end:
    np.dot(a, b)
    iters += 1
print(iters / ${dur})
" >"$bench_out" 2>/dev/null &
            BENCH_PID=$! ;;
        membw)
            # 4 workers in parallel; each prints its own iters/s — summed by parser
            ( for _w in 1 2 3 4; do $TS python3 -c "
import numpy as np, time
n = 32*1024*1024
a = np.ones(n, dtype=np.float32)
b = np.random.rand(n).astype(np.float32)
c = np.random.rand(n).astype(np.float32)
end = time.monotonic() + ${dur}
iters = 0
while time.monotonic() < end:
    np.multiply(b, 2.0, out=a)
    np.add(a, c, out=a)
    iters += 1
print(iters / ${dur})
" >>"$bench_out" 2>/dev/null & done; wait ) &
            BENCH_PID=$! ;;
        mem_latency)
            $TS python3 -c "
import numpy as np, time
arr = np.random.rand(32*1024*1024)
end = time.monotonic() + ${dur}
iters = 0
while time.monotonic() < end:
    _ = arr[np.random.randint(0, len(arr), 50000)].sum()
    iters += 1
print(iters / ${dur})
" >"$bench_out" 2>/dev/null &
            BENCH_PID=$! ;;
        compile)
            ( end_t=$(( SECONDS + dur )); iters=0
              while [ $SECONDS -lt $end_t ]; do
                  $TS make -C "$DC_DIR" clean >/dev/null 2>&1
                  $TS make -C "$DC_DIR" -j4   >/dev/null 2>&1 || true
                  iters=$(( iters + 1 ))
              done
              python3 -c "print($iters / $dur)" ) >"$bench_out" 2>/dev/null &
            BENCH_PID=$! ;;
        inference)
            $TS python3 -c "
import sys, glob, time
sys.path.insert(0, '$TRAINING_DIR')
import torch
from model import FreqMLP
models = sorted(glob.glob('$TRAINING_DIR/model_fp32_rl_*.pt'),
                key=lambda x: int(x.rsplit('_',1)[-1].replace('.pt','')))
path = models[-1] if models else '$TRAINING_DIR/model_fp32.pt'
net = FreqMLP(); net.load_state_dict(torch.load(path, map_location='cpu')); net.eval()
batch = torch.randn(64, 174)
end = time.monotonic() + ${dur}
iters = 0
with torch.no_grad():
    while time.monotonic() < end:
        net(batch); iters += 1
print(iters * 64 / ${dur})
" >"$bench_out" 2>/dev/null &
            BENCH_PID=$! ;;
    esac
}

# ── throughput parser ─────────────────────────────────────────────────────────
# stress-ng --metrics-brief writes to stderr:
#   stress-ng: info:  [N] <stressor>  <bogo-ops>  <real-s>  <usr-s>  <sys-s>  <bogo-ops/s>  <bogo-ops/s>
# Python benchmarks print a float (iters/s per worker) to stdout.

parse_throughput() {
    local name=$1 bench_out=$2 dur=$3
    python3 - "$name" "$bench_out" "$dur" << 'PYEOF'
import sys, re

STRESSOR = {
    'cpu_all':    ['cpu'],
    'single_core':['cpu'],
    'stream':     ['stream'],
    'branch':     ['branch'],
    'icache':     ['icache'],
    'bursty':     ['cpu'],
    # inference handled by float-sum branch below
}

name     = sys.argv[1]
path     = sys.argv[2]
duration = float(sys.argv[3])

try:
    content = open(path).read()
except Exception:
    print(0); sys.exit()

if name in STRESSOR:
    stressors  = STRESSOR[name]
    total_bogo = 0.0
    total_secs = 0.0
    for line in content.splitlines():
        if 'info' not in line:
            continue
        parts = line.split()
        for i, p in enumerate(parts):
            if p in stressors and i + 2 < len(parts):
                try:
                    total_bogo += float(parts[i + 1])
                    total_secs += float(parts[i + 2])
                except ValueError:
                    pass
                break
    if total_secs > 0:
        # Divide by full duration for bursty so idle phases show up in the efficiency ratio.
        if name in ('bursty',):
            print(f'{total_bogo / duration:.2f}')
        else:
            print(f'{total_bogo / total_secs:.2f}')
    else:
        print(0)

else:
    # Python benchmarks: each worker prints one float (iters/s). Sum them.
    vals = []
    for line in content.splitlines():
        try:
            vals.append(float(line.strip()))
        except ValueError:
            pass
    print(f'{sum(vals):.4f}' if vals else '0')
PYEOF
}

# ── pmic + sysfs power & freq sampler ────────────────────────────────────────

sample_pmic() {
    local dur=$1
    python3 - "$dur" << 'EOF'
import sys, time, struct, os

dur = float(sys.argv[1])
end = time.monotonic() + dur
freqs, vdds = [], []

while time.monotonic() < end:
    try:
        fd = os.open("/dev/pmic_driver_novel", os.O_RDONLY)
        data = os.read(fd, 14)
        os.close(fd)
        if len(data) != 14:
            continue
        _, _, _, _, ch3_shunt, ch3_bus, _ = struct.unpack(">7h", data)
        vdd_in_mw = (ch3_bus * abs(ch3_shunt)) // 10000
        vdds.append(vdd_in_mw)
    except Exception:
        pass

    try:
        with open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r") as f:
            freqs.append(int(f.read().strip()))
    except Exception:
        pass

    time.sleep(0.1)
avg_freq = sum(freqs) // len(freqs) if freqs else 0
avg_vdd = sum(vdds) // len(vdds) if vdds else 0
print(f"{avg_freq} {avg_vdd}")
EOF
}
# ── main loop ─────────────────────────────────────────────────────────────────

echo "governor,benchmark,avg_freq_khz,avg_power_mw,throughput,efficiency,unit" > "$CSV"

# ML governor runs first.
GOVERNORS_TO_TEST=("ml_governor" "${LINUX_GOVERNORS[@]}")
total=$(( ${#GOVERNORS_TO_TEST[@]} * ${#BENCHMARKS[@]} ))
run=0
first_gov=1

for gov in "${GOVERNORS_TO_TEST[@]}"; do
    if [ "$first_gov" -eq 0 ]; then
        log "waiting for tj-thermal ≤ 52°C ..."
        while true; do
            tj=$(( $(cat /sys/class/thermal/thermal_zone8/temp) / 1000 ))
            [ "$tj" -le 52 ] && break
            log "  tj=${tj}°C — cooling down ..."
            sleep 15
        done
        log "tj=${tj}°C — starting next governor"
    fi
    first_gov=0

    if [ "$gov" = "ml_governor" ]; then
        log "=== ML governor ==="
        start_ml_governor
    else
        log "=== Linux: $gov ==="
        stop_ml_governor
        set_linux_governor "$gov"
    fi
    sleep "$SETTLE"

    for bench in "${BENCHMARKS[@]}"; do
        run=$(( run + 1 ))
        log "[$run/$total] $gov / $bench"

        bench_out=$(mktemp)
        start_bench "$bench" "$DURATION" "$bench_out"
        read -r freq vdd < <(sample_pmic "$DURATION")
        wait "$BENCH_PID" 2>/dev/null || true; BENCH_PID=""

        throughput=$(parse_throughput "$bench" "$bench_out" "$DURATION")
        rm -f "$bench_out"

        efficiency=0
        if [ -n "$vdd" ] && [ "$vdd" -gt 0 ] && [ -n "$throughput" ]; then
            efficiency=$(python3 -c "
t=float('${throughput}' or 0)
p=float('${vdd}' or 1)
print(f'{t/p:.6f}' if p > 0 and t > 0 else '0')")
        fi

        case "$bench" in
            matmul)      unit="matmul/s/mW"  ;;
            membw)       unit="iter/s/mW"    ;;
            mem_latency) unit="iter/s/mW"    ;;
            compile)     unit="builds/s/mW"  ;;
            inference)   unit="infer/s/mW"    ;;
            *)           unit="bogo-ops/s/mW" ;;
        esac

        log "  freq=${freq}kHz  power=${vdd}mW  throughput=${throughput}  eff=${efficiency} ${unit}"
        echo "$gov,$bench,$freq,$vdd,$throughput,$efficiency,$unit" >> "$CSV"

        sleep "$COOLDOWN"
    done
done

stop_ml_governor

# ── summary table ─────────────────────────────────────────────────────────────

log "=== results ==="
python3 - "$CSV" << 'EOF'
import csv, sys
from collections import defaultdict

rows    = list(csv.DictReader(open(sys.argv[1])))
benches = list(dict.fromkeys(r['benchmark'] for r in rows))
govs    = list(dict.fromkeys(r['governor']  for r in rows))
data    = {(r['governor'], r['benchmark']): r for r in rows}

COL = 14

def hdr():
    print(f"\n{'benchmark':<16}", end='')
    for g in govs: print(f"{g:>{COL}}", end='')
    print()
    print('-' * (16 + COL * len(govs)))

hdr()
print("\n--- avg power (mW) ---")
for b in benches:
    print(f"{b:<16}", end='')
    for g in govs: print(f"{data.get((g,b),{}).get('avg_power_mw','-'):>{COL}}", end='')
    print()

hdr()
print("\n--- avg freq (kHz) ---")
for b in benches:
    print(f"{b:<16}", end='')
    for g in govs: print(f"{data.get((g,b),{}).get('avg_freq_khz','-'):>{COL}}", end='')
    print()

hdr()
print("\n--- throughput (benchmark-native units) ---")
for b in benches:
    print(f"{b:<16}", end='')
    for g in govs:
        v = data.get((g,b),{}).get('throughput','0')
        try:    print(f"{float(v):>{COL}.1f}", end='')
        except: print(f"{'?':>{COL}}", end='')
    print()

hdr()
print("\n--- efficiency ratio vs schedutil  (>1.0 = ML wins) ---")
for b in benches:
    ref_eff = float(data.get(('schedutil', b), {}).get('efficiency', 0) or 0)
    print(f"{b:<16}", end='')
    for g in govs:
        eff = float(data.get((g, b), {}).get('efficiency', 0) or 0)
        if g == 'schedutil':
            print(f"{'1.000':>{COL}}", end='')
        elif ref_eff > 0 and eff > 0:
            ratio = eff / ref_eff
            tag = ' ✓' if ratio >= 1.0 else '  '
            print(f"{ratio:>{COL-2}.3f}{tag}", end='')
        else:
            print(f"{'?':>{COL}}", end='')
    print()

print("\n--- WINNER per benchmark ---")
ml_wins = 0
for b in benches:
    ref_eff = float(data.get(('schedutil', b), {}).get('efficiency', 0) or 0)
    ml_eff  = float(data.get(('ml_governor', b), {}).get('efficiency', 0) or 0)
    if ref_eff > 0 and ml_eff > 0:
        ratio = ml_eff / ref_eff
        if ratio >= 1.0:
            ml_wins += 1
            print(f"  {b:<16} → ml_governor  ({ratio:.3f}x schedutil)")
        else:
            print(f"  {b:<16} → schedutil    (ml ratio={ratio:.3f})")
    else:
        print(f"  {b:<16} → ?  (missing data)")

total_benches = len(benches)
print(f"\nML governor wins {ml_wins}/{total_benches} benchmarks vs schedutil")
EOF

log "results saved to $CSV"
