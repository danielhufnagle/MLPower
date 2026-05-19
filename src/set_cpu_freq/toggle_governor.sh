#!/usr/bin/env bash

STATE_FILE="/tmp/jetson_cpufreq_state"
CPU_PATH="/sys/devices/system/cpu"

TARGET_FREQ=${1:-1500000}  # default 1.5 GHz (kHz)

set_userspace() {
    echo "Switching to userspace and setting freq to $TARGET_FREQ kHz"

    # Save current governor (assume all CPUs use same one)
    OLD_GOV=$(cat $CPU_PATH/cpu0/cpufreq/scaling_governor)

    echo "$OLD_GOV" > "$STATE_FILE"

    # Set userspace governor
    for cpu in $CPU_PATH/cpu[0-9]*; do
        echo userspace | sudo tee $cpu/cpufreq/scaling_governor > /dev/null
    done

    # Set frequency
    for cpu in $CPU_PATH/cpu[0-9]*; do
        echo $TARGET_FREQ | sudo tee $cpu/cpufreq/scaling_setspeed > /dev/null
    done

    echo "Done. Previous governor saved as: $OLD_GOV"
}

restore_governor() {
    if [ ! -f "$STATE_FILE" ]; then
        echo "No saved state found. Nothing to restore."
        exit 1
    fi

    OLD_GOV=$(cat "$STATE_FILE")
    echo "Restoring governor: $OLD_GOV"

    for cpu in $CPU_PATH/cpu[0-9]*; do
        echo $OLD_GOV | sudo tee $cpu/cpufreq/scaling_governor > /dev/null
    done

    rm -f "$STATE_FILE"
    echo "Restored."
}

# Toggle behavior
if [ -f "$STATE_FILE" ]; then
    restore_governor
else
    set_userspace
fi