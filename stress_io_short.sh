#!/usr/bin/env bash
set -euo pipefail

JOBS="${JOBS:-4}"

echo "[stress-short] AHCI fault-injection pass"
JOBS="$JOBS" LOOPS="${LOOPS:-8}" QEMU_TIMEOUT="${QEMU_TIMEOUT:-45}" STEP="${STEP:-0.015}" \
  ./stress_ahci.sh

echo "[stress-short] NVMe crash/replay pass"
JOBS="$JOBS" PHASE1_TIMEOUT="${PHASE1_TIMEOUT:-12}" PHASE2_TIMEOUT="${PHASE2_TIMEOUT:-16}" STEP="${STEP_NVME:-0.02}" \
  ./stress_nvme.sh

echo "[stress-short] PASS"
