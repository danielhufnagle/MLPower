#!/bin/bash

set -e

if [ "$EUID" -ne 0 ]; then
  echo "Error: Must run as root."
  exit 1
fi

# Ensure MaxN power mode for Orin Nano
NVPMODEL_MODE=$(nvpmodel -q 2>/dev/null | grep "NV Power Mode" | awk '{print $NF}')
if [ "$NVPMODEL_MODE" != "MAXN_SUPER" ]; then
    echo "Setting nvpmodel to MaxN..."
    nvpmodel -m 2
fi

RESULTS_DIR="results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"
CSV_LOG="$RESULTS_DIR/telemetry.csv"

echo "timestamp,governor,workload,runtime_s,avg_power_mw,avg_temp_c,energy_j,edp,throughput,throughput_per_joule,instructions_per_joule,throughput_unit" > "$CSV_LOG"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COREMARK_EXE="$SCRIPT_DIR/../../coremark/coremark.exe"

# Check required tools before starting a long run
for tool in stress-ng openssl sysbench 7z ffmpeg python3 bc taskset tegrastats; do
  if ! command -v "$tool" &>/dev/null; then
    echo "Error: '$tool' not found. Install missing tools and retry."
    echo "  apt install sysbench lmbench p7zip-full ffmpeg"
    exit 1
  fi
done
if [ ! -x "$COREMARK_EXE" ]; then
  echo "Error: coremark not built at $COREMARK_EXE. Run 'make' in coremark/ first."
  exit 1
fi

ML_GOV_KO="$SCRIPT_DIR/../ML_GOVERNOR/ml_gov.ko"
PMU_PROFILER_KO="$SCRIPT_DIR/../data_collection/pmu_profiler.ko"
PMIC_DRIVER_KO="$SCRIPT_DIR/../pmic_driver/pmic_driver.ko"
PMU_DATA_CSV="/home/mlpower/MLPower/src/data_collection/pmu_data.csv"

AVAILABLE_GOVS=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors 2>/dev/null)
GOVERNORS=()
for g in performance ondemand conservative schedutil; do
  if echo "$AVAILABLE_GOVS" | grep -qw "$g"; then
    GOVERNORS+=("$g")
  else
    echo "Warning: governor '$g' not available on this kernel, skipping."
  fi
done
GOVERNORS+=("ml_governor")

for ko in "$ML_GOV_KO" "$PMU_PROFILER_KO" "$PMIC_DRIVER_KO"; do
  if [ ! -f "$ko" ]; then
    echo "Error: kernel module not found: $ko"
    exit 1
  fi
done

# Load pmu_profiler once for instructions/J measurement across all governors
lsmod | grep -q pmu_profiler || insmod "$PMU_PROFILER_KO" || { echo "Error: failed to insmod pmu_profiler.ko"; exit 1; }

DURATION=8
REPETITIONS=1

declare -A WORKLOADS=(
  ["cpu_stress"]="taskset -c 0-3 stress-ng --cpu 4 --timeout ${DURATION}s --metrics-brief"
  ["mem_stream"]="taskset -c 0-3 stress-ng --stream 4 --timeout ${DURATION}s --metrics-brief"
  ["matmul"]="taskset -c 0-3 python3 -c \"import numpy as np,time; a=np.random.rand(4096,4096); e=time.monotonic()+${DURATION}; ops=sum(1 for _ in iter(lambda: time.monotonic()<e, False) if np.dot(a,a) is not None); print(ops)\""
  ["stress_branch"]="taskset -c 0-3 stress-ng --branch 4 --timeout ${DURATION}s --metrics-brief"
  ["ffmpeg"]="taskset -c 0-3 ffmpeg -f lavfi -i testsrc=size=1920x1080:rate=30 -t ${DURATION} -c:v libx264 -preset medium -f null /dev/null -y"
  ["bursty"]="bash -c 'taskset -c 0-3 stress-ng --cpu 4 --timeout 3s --metrics-brief; sleep 2; taskset -c 0-3 stress-ng --cpu 4 --timeout 3s --metrics-brief'"
)

# --- Telemetry Functions ---
start_telemetry() {
  local gov=$1 wl=$2 run=$3
  local out_dir="$RESULTS_DIR/${gov}_${wl}_run${run}"
  mkdir -p "$out_dir"

  # Power and thermals via tegrastats (100ms interval)
  tegrastats --interval 100 > "$out_dir/tegrastats.log" &
  TEGRA_PID=$!

  # High-resolution frequency trace
  while true; do
    ts=$(date +%s.%N)
    freq=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo "0")
    echo "$ts,$freq" >> "$out_dir/freq_trace.csv"
    sleep 0.01
  done &
  FREQ_PID=$!
}

stop_telemetry() {
  kill $TEGRA_PID 2>/dev/null || true
  kill $FREQ_PID 2>/dev/null || true
  wait $TEGRA_PID 2>/dev/null || true
  wait $FREQ_PID 2>/dev/null || true
}

parse_and_log_run() {
  local gov=$1 wl=$2 run=$3 runtime=$4 pre_pmu_lines=$5
  local out_dir="$RESULTS_DIR/${gov}_${wl}_run${run}"

  # Extract avg power (VDD_IN instant reading) and Tj temp from tegrastats
  local avg_power=$(grep -oP 'VDD_IN \K[0-9]+(?=mW)' "$out_dir/tegrastats.log" | awk '{s+=$1;n++} END{print (n>0)?s/n:0}')
  local avg_temp=$(grep -oP 'tj@\K[0-9.]+(?=C)' "$out_dir/tegrastats.log" | awk '{s+=$1;n++} END{print (n>0)?s/n:0}')

  # Compute Energy (Joules) and EDP
  local energy=$(echo "scale=4; ($avg_power / 1000) * $runtime" | bc)
  local edp=$(echo "scale=4; $energy * $runtime" | bc)

  # Parse workload-native throughput
  local throughput=0 throughput_unit="n/a"
  case "$wl" in
    cpu_stress|mem_stream|stress_branch|bursty)
      # stress-ng --metrics-brief: data line ends with "real-bogo-ops/s  usr-bogo-ops/s"
      # $(NF-1) is the real-time bogo-ops/s; filter to lines ending in two floats
      throughput=$(awk '/info:/ && /[0-9]+\.[0-9]+$/ && !/stressor/ {print $(NF-1)}' "$out_dir/workload_out.txt" 2>/dev/null | tail -1)
      throughput_unit="bogo-ops/s"
      ;;
    ffmpeg)
      throughput=$(grep -oP 'frame=\s*\K[0-9]+' "$out_dir/workload_out.txt" 2>/dev/null | tail -1)
      throughput_unit="frames"
      ;;
    matmul)
      throughput=$(grep -E '^[0-9]+$' "$out_dir/workload_out.txt" 2>/dev/null | tail -1)
      throughput_unit="matmuls"
      ;;
  esac
  throughput=${throughput:-0}
  local tpj=$(echo "scale=4; if ($energy > 0 && $throughput > 0) $throughput / $energy else 0" | bc 2>/dev/null || echo 0)

  # Instructions/J from pmu_profiler (inst_retired cols 4,11,18,25,32,39 = all 6 cores)
  local total_inst=0
  if [ -f "$PMU_DATA_CSV" ] && [ "$pre_pmu_lines" -gt 0 ]; then
    total_inst=$(awk -F',' -v pre="$pre_pmu_lines" \
      'NR>pre {s+=$4+$11+$18+$25+$32+$39} END{printf "%.0f\n", s}' "$PMU_DATA_CSV")
  fi
  total_inst=${total_inst:-0}
  local ipj=0
  if [ "${total_inst:-0}" -gt 0 ] 2>/dev/null && [ "$(echo "$energy > 0" | bc)" = "1" ]; then
    ipj=$(echo "scale=2; $total_inst / $energy" | bc 2>/dev/null || echo 0)
  fi

  echo "$(date +%s),$gov,$wl,$runtime,$avg_power,$avg_temp,$energy,$edp,$throughput,$tpj,$ipj,$throughput_unit" >> "$CSV_LOG"
  echo "  -> Power: ${avg_power}mW | Temp: ${avg_temp}°C | EDP: $edp | Tput/J: $tpj $throughput_unit/J | Inst/J: $ipj"
}

# --- Execution Loop ---
for GOV in "${GOVERNORS[@]}"; do
  echo "========================================"
  echo "Configuring Governor: $GOV"
  
  if [ "$GOV" == "ml_governor" ]; then
    # pmu_profiler already loaded above; just need pmic_driver + ml_gov
    lsmod | grep -q pmic_driver || insmod "$PMIC_DRIVER_KO" || { echo "Error: failed to insmod pmic_driver.ko"; exit 1; }
    insmod "$ML_GOV_KO" || { echo "Error: failed to insmod ml_gov.ko"; exit 1; }
    for policy in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do
        echo "userspace" > "$policy" 2>/dev/null || true
    done
  else
    # Ensure ml_gov is not loaded when testing other governors
    rmmod ml_gov 2>/dev/null || true
    for policy in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do
        echo "$GOV" > "$policy" 2>/dev/null || true
    done
  fi
  sleep 2 # Allow thermals to settle

  for WL_NAME in "${!WORKLOADS[@]}"; do
    CMD="${WORKLOADS[$WL_NAME]}"
    
    for ((r=1; r<=REPETITIONS; r++)); do
      echo "Running [$GOV] | Workload: $WL_NAME | Run $r/$REPETITIONS"
      out_dir="$RESULTS_DIR/${GOV}_${WL_NAME}_run${r}"
      start_telemetry "$GOV" "$WL_NAME" "$r"

      # Snapshot pmu_data.csv line count so we only sum this run's instructions
      pre_pmu_lines=$(wc -l < "$PMU_DATA_CSV" 2>/dev/null || echo 0)

      # Execute workload
      START_TIME=$(date +%s.%N)
      eval "$CMD" > "$out_dir/workload_out.txt" 2>&1 || echo "  Warning: workload $WL_NAME exited non-zero (continuing)"
      END_TIME=$(date +%s.%N)

      stop_telemetry

      RUNTIME=$(echo "$END_TIME - $START_TIME" | bc)
      parse_and_log_run "$GOV" "$WL_NAME" "$r" "$RUNTIME" "$pre_pmu_lines"
      
      sleep 2 # Cooldown
    done
  done

  if [ "$GOV" == "ml_governor" ]; then
    rmmod ml_gov      2>/dev/null || true
    rmmod pmic_driver 2>/dev/null || true
  fi
done

rmmod pmu_profiler 2>/dev/null || true

echo "Execution complete. Handoff to Python for analysis..."
chmod -R a+w "$RESULTS_DIR"
sudo -u mlpower python3 "$SCRIPT_DIR/analyze.py" "$RESULTS_DIR"
