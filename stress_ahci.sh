#!/usr/bin/env bash
set -euo pipefail

JOBS="${JOBS:-4}"
STEP="${STEP:-0.02}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-90}"
LOOPS="${LOOPS:-24}"
LOG_TAIL_LINES="${LOG_TAIL_LINES:-200}"
RESTORE_BUILD="${RESTORE_BUILD:-1}"

OUT_NORMAL="$(mktemp)"
OUT_FAULT="$(mktemp)"
DISK="$(mktemp)"
SEED=""
EXT4_EXPECT=0

cleanup() {
  rm -f "$OUT_NORMAL" "$OUT_FAULT" "$DISK"
  if [[ -n "$SEED" ]]; then
    rm -rf "$SEED"
  fi
}
trap cleanup EXIT

if command -v mke2fs >/dev/null 2>&1; then
  SEED="$(mktemp -d)"
  mkdir -p "$SEED/base"
  printf "stress seed\n" > "$SEED/base/readme.txt"
  truncate -s 128M "$DISK"
  if mke2fs -q -F -t ext4 -b 4096 -m 0 \
      -O extent,filetype,^has_journal,^64bit,^metadata_csum,^dir_index \
      -d "$SEED" "$DISK" 32512; then
    EXT4_EXPECT=1
  fi
else
  truncate -s 128M "$DISK"
fi

run_case() {
  local out="$1"
  (
    emit() {
      echo "$1"
      sleep "$STEP"
    }

    sleep 0.6
    emit 'ls /dev'
    if [[ "$EXT4_EXPECT" -eq 1 ]]; then
      emit 'mount /dev/sda /mnt ext4'
      emit 'ls /mnt'
    fi

    for i in $(seq 1 "$LOOPS"); do
      if [[ "$EXT4_EXPECT" -eq 1 ]]; then
        emit "echo line-$i >> /mnt/stress.log"
      else
        emit "echo line-$i > /dev/sda"
      fi
      if (( i % 12 == 0 )); then
        if [[ "$EXT4_EXPECT" -eq 1 ]]; then
          emit 'cat /mnt/stress.log'
        else
          emit 'cat /dev/sda'
        fi
      fi
    done

    if [[ "$EXT4_EXPECT" -eq 1 ]]; then
      emit 'cp /mnt/stress.log /mnt/stress2.log'
      emit 'cat /mnt/stress2.log'
      emit 'mv /mnt/stress2.log /mnt/stress3.log'
      emit 'rm /mnt/stress3.log'
      emit 'umount /mnt'
    fi

    emit 'echo stress-done'
    emit 'exit'
    sleep 0.4
  ) | timeout "$QEMU_TIMEOUT" qemu-system-aarch64 \
        -machine virt,virtualization=on,gic-version=2 \
        -cpu cortex-a53 \
        -m 256M \
        -smp 1 \
        -nographic \
        -monitor none \
        -drive if=none,file="$DISK",format=raw,id=sd0 \
        -device ich9-ahci,id=ahci0 \
        -device ide-hd,drive=sd0,bus=ahci0.0 \
        -device loader,file=build/kernel.elf,cpu-num=0 >"$out" 2>&1 || true
}

run_make() {
  local log
  log="$(mktemp)"
  if ! make "$@" >"$log" 2>&1; then
    cat "$log" >&2
    rm -f "$log"
    return 1
  fi
  rm -f "$log"
  return 0
}

print_log() {
  local out="$1"
  if [[ "${VERBOSE:-0}" == "1" ]]; then
    cat "$out"
  else
    tail -n "$LOG_TAIL_LINES" "$out"
  fi
}

echo "[stress] build normal"
run_make -j"$JOBS"
run_case "$OUT_NORMAL"
print_log "$OUT_NORMAL"

grep -q "\[ahci\] ready" "$OUT_NORMAL"
grep -q "mode=irq" "$OUT_NORMAL"
grep -q "stress-done" "$OUT_NORMAL"
if rg -q "\[panic\]|fork failed|no runnable" "$OUT_NORMAL"; then
  echo "[stress] FAIL: normal run runtime failure" >&2
  exit 1
fi

echo "[stress] build fault-injection"
run_make clean
run_make -j"$JOBS" KERNEL_DEFS='-DAHCI_FAULT_TIMEOUT_EVERY=9 -DAHCI_FAULT_ERROR_EVERY=11 -DAHCI_FAULT_RESET_STORM_EVERY=13 -DAHCI_FAULT_PARTIAL_EVERY=17'
run_case "$OUT_FAULT"
print_log "$OUT_FAULT"

grep -q "\[ahci\] ready" "$OUT_FAULT"
grep -q "mode=irq" "$OUT_FAULT"
grep -q "\[ahci\]\[fault\]" "$OUT_FAULT"
if rg -q "\[panic\]|no runnable" "$OUT_FAULT"; then
  echo "[stress] FAIL: fault-injection run kernel failure" >&2
  exit 1
fi

if [[ "$RESTORE_BUILD" == "1" ]]; then
  echo "[stress] restore normal build"
  run_make clean
  run_make -j"$JOBS"
else
  echo "[stress] skip normal rebuild (set RESTORE_BUILD=1 to restore now)"
fi

echo "[stress] PASS"
