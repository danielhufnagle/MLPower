#!/bin/bash
# Deploy script: unload, rebuild, and load pmu_profiler module

set -e

echo "=== Unloading old module ==="
sudo rmmod pmu_profiler 2>/dev/null || true

echo "=== Building module ==="
make

echo "=== Loading new module ==="
sudo insmod pmu_profiler.ko

echo "=== Done! Module loaded ==="
sudo dmesg | tail -5
