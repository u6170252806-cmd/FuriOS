#!/usr/bin/env bash
set -euo pipefail

JOBS="${JOBS:-4}"
STEP="${STEP:-0.03}"
PHASE1_TIMEOUT="${PHASE1_TIMEOUT:-16}"
PHASE2_TIMEOUT="${PHASE2_TIMEOUT:-24}"
DISK_SIZE="${DISK_SIZE:-128M}"

OUT1="$(mktemp)"
OUT2="$(mktemp)"
DISK="$(mktemp)"

cleanup() {
  rm -f "$OUT1" "$OUT2" "$DISK"
}
trap cleanup EXIT

make -j"$JOBS"
truncate -s "$DISK_SIZE" "$DISK"

run_qemu() {
  local timeout_secs="$1"
  local out="$2"
  timeout "$timeout_secs" qemu-system-aarch64 \
    -machine virt,virtualization=on,gic-version=2 \
    -cpu cortex-a53 \
    -m 256M \
    -smp 1 \
    -nographic \
    -monitor none \
    -device nvme,id=nvmec0,serial=StressNVMe0,bus=pcie.0,addr=0x6 \
    -drive if=none,file="$DISK",format=raw,id=nvme0_1 \
    -device nvme-ns,drive=nvme0_1,bus=nvmec0,nsid=1 \
    -device loader,file=build/kernel.elf,cpu-num=0 >"$out" 2>&1 || true
}

(
  emit() { echo "$1"; sleep "$STEP"; }
  sleep 0.8
  emit 'mkfs.ext4 /dev/nvme0n1'
  emit 'mount /dev/nvme0n1 /mnt ext4'
  for i in $(seq 1 8); do
    emit "echo crash-$i >> /mnt/journal.log"
  done
  emit 'cat /mnt/journal.log'
  emit 'sleep 2'
) | run_qemu "$PHASE1_TIMEOUT" "$OUT1"

cat "$OUT1"
grep -q "\[nvme\] ready" "$OUT1"
grep -q "mkfs.ext4: formatted" "$OUT1"
grep -q "\[ext4\] mounted rw at /mnt" "$OUT1"
grep -q "crash-8" "$OUT1"
if rg -q "\[panic\]|no runnable|fork failed" "$OUT1"; then
  echo "[stress-nvme] FAIL: phase1 runtime failure" >&2
  exit 1
fi

(
  emit() { echo "$1"; sleep "$STEP"; }
  sleep 0.8
  emit 'umount /mnt'
  emit 'fsck.ext4 -p /dev/nvme0n1'
  emit 'mount /dev/nvme0n1 /mnt ext4'
  emit 'cat /mnt/journal.log'
  emit 'echo post-recover >> /mnt/journal.log'
  emit 'cat /mnt/journal.log'
  emit 'umount /mnt'
  emit 'exit'
  sleep 0.3
) | run_qemu "$PHASE2_TIMEOUT" "$OUT2"

cat "$OUT2"
grep -q "\[nvme\] ready" "$OUT2"
grep -q "\[ext4\] mounted rw at /mnt" "$OUT2"
grep -q "post-recover" "$OUT2"
grep -q "\[ext4\] unmounted /mnt" "$OUT2"
if ! rg -q "fsck.ext4: clean|fsck.ext4: repaired" "$OUT2"; then
  echo "[stress-nvme] FAIL: fsck did not report clean/repaired" >&2
  exit 1
fi
if rg -q "\[panic\]|no runnable|fork failed" "$OUT2"; then
  echo "[stress-nvme] FAIL: phase2 runtime failure" >&2
  exit 1
fi

echo "[stress-nvme] PASS"
