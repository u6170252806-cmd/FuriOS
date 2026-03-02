#!/usr/bin/env bash
set -euo pipefail

make -j4

STEP="${STEP:-0.02}"
OUT="$(mktemp)"
DISK="$(mktemp)"
OUT_AHCI=""
DISK_AHCI1=""
DISK_AHCI2=""
SEED=""
EXT4_EXPECT=0

if command -v mke2fs >/dev/null 2>&1; then
  SEED="$(mktemp -d)"
  mkdir -p "$SEED/docs"
  mkdir -p "$SEED/empty"
  printf "hello from ext4\n" > "$SEED/hello.txt"
  printf "FuriOS ext4 root\n" > "$SEED/docs/readme.txt"
  ln -sf docs/readme.txt "$SEED/readme-link"
  truncate -s 64M "$DISK"
  if mke2fs -q -F -t ext4 -b 4096 -m 0 \
      -O extent,filetype,^has_journal,^64bit,^metadata_csum,^dir_index \
      -d "$SEED" "$DISK" 16128; then
    EXT4_EXPECT=1
  fi
else
  truncate -s 64M "$DISK"
fi

trap 'rm -f "${OUT:-}" "${DISK:-}" "${OUT_AHCI:-}" "${DISK_AHCI1:-}" "${DISK_AHCI2:-}"; if [[ -n "${SEED:-}" ]]; then rm -rf "$SEED"; fi' EXIT

(
  emit() {
    echo "$1"
    sleep "$STEP"
  }

  sleep 0.8
  emit 'ls'
  emit 'ls /dev'
  emit 'echo dev-null > /dev/null'
  emit 'echo dev-tty > /dev/tty'
  emit 'kill -15 1'
  emit 'kill 9999'
  emit 'cd /bin'
  emit 'ls'
  emit 'cd /'
  emit 'pwd'
  emit 'mkdir /tmpx'
  emit 'mkdir /tmpx/d'
  emit 'touch /tmpx/a'
  emit 'cp /etc/motd /tmpx/m'
  emit 'mv /tmpx/m /tmpx/m2'
  emit 'cat /tmpx/m2'
  emit 'rm -rf /tmpx'
  emit 'ls /tmpx'
  emit 'cat /etc/motd'

  if [[ "$EXT4_EXPECT" -eq 1 ]]; then
    emit 'ls /mnt'
    emit 'cat /mnt/hello.txt'
    emit 'ls /mnt/docs'
    emit 'cat /mnt/docs/readme.txt'
    emit 'cat /mnt/readme-link'
    emit 'ln /mnt/hello.txt /mnt/hello.hard'
    emit 'cat /mnt/hello.hard'
    emit 'ln -s docs/readme.txt /mnt/docs-link'
    emit 'cat /mnt/docs-link'
    emit 'rm /mnt/hello.hard'
    emit 'rm /mnt/docs-link'
    emit 'echo extent-native > /mnt/hello.txt'
    emit 'echo extent-tail >> /mnt/hello.txt'
    emit 'cat /mnt/hello.txt'
    emit 'echo ext4-write > /mnt/new.txt'
    emit 'echo ext4-append >> /mnt/new.txt'
    emit 'cat /mnt/new.txt'
    emit 'echo ext4-reset > /mnt/new.txt'
    emit 'cat /mnt/new.txt'
    emit 'mv /mnt/new.txt /mnt/new2.txt'
    emit 'cat /mnt/new2.txt'
    emit 'rm /mnt/new2.txt'
    emit 'rmdir /mnt/empty'
    emit 'mkdir /mnt/hi'
    emit 'touch /mnt/hi/file1'
    emit 'ls /mnt/hi'
    emit 'rm /mnt/hi/file1'
    emit 'rmdir /mnt/hi'
    emit 'ls /mnt'
    emit 'cd /mnt'
    emit 'umount /mnt'
    emit 'cd /'
    emit 'umount /mnt'
    emit 'mount /dev/vda /mnt ext4'
    emit 'ls /mnt'
  fi

  emit 'echo "hello world" > /q1'
  emit 'cat < /q1'
  emit 'echo foo\ bar'
  emit 'echo hi | cat'
  emit 'cat /etc/motd | cat > /p1'
  emit 'cat /p1'
  emit 'echo one two | cat | cat'
  emit 'echo append-one > /app'
  emit 'echo append-two >> /app'
  emit 'cat /app'
  emit 'umount /mnt'
  emit 'mkfs.ext4 -L FuriOSVOL -O extents,64bit,sparse_super -m 1 -E stride=8 /dev/vda'
  emit 'fsck.ext4 /dev/vda'
  emit 'mount /dev/vda /mnt ext4'
  emit 'ls /mnt'
  emit 'mkdir /mnt/mkfst'
  emit 'touch /mnt/mkfst/a'
  emit 'ls /mnt/mkfst'
  emit 'sleep 1'
  emit 'echo slept-ok'
  emit 'echo io-start > /stress'
  for i in $(seq 1 6); do
    emit "echo line-$i >> /stress"
  done
  emit 'cat /stress | cat > /stress2'
  emit 'cat /stress2'
  emit 'rm /p1'
  emit 'rm /q1'
  emit 'rm /app'
  emit 'rm /stress'
  emit 'rm /stress2'
  emit 'echo smoke-test'
  emit 'exit'
  sleep 0.4
) | timeout 70 qemu-system-aarch64 \
      -machine virt,virtualization=on,gic-version=2 \
      -cpu cortex-a53 \
      -m 256M \
      -smp 1 \
      -nographic \
      -monitor none \
      -drive if=none,file="$DISK",format=raw,id=vd0 \
      -device virtio-blk-device,drive=vd0 \
      -device loader,file=build/kernel.elf,cpu-num=0 >"$OUT" 2>&1 || true

cat "$OUT"

grep -q "FuriOS aarch64 boot" "$OUT"
grep -q "\[block-cache\] selftest ok" "$OUT"
grep -q "sh\$ ls" "$OUT"
grep -q "sh\$ ls /dev" "$OUT"
grep -q "null" "$OUT"
grep -q "zero" "$OUT"
grep -q "tty" "$OUT"
grep -q "vda" "$OUT"
grep -q "init" "$OUT"
grep -q "echo" "$OUT"
grep -q "FuriOS EL0 userspace online" "$OUT"

if [[ "$EXT4_EXPECT" -eq 1 ]]; then
  grep -q "\[ext4\] mounted rw at /mnt" "$OUT"
  grep -q "hello from ext4" "$OUT"
  grep -q "FuriOS ext4 root" "$OUT"
  grep -q "sh\$ cat /mnt/readme-link" "$OUT"
  grep -q "sh\$ cat /mnt/hello.hard" "$OUT"
  grep -q "sh\$ cat /mnt/docs-link" "$OUT"
  rg -q "^extent-native$" "$OUT"
  rg -q "^extent-tail$" "$OUT"
  grep -q "ext4-write" "$OUT"
  grep -q "ext4-append" "$OUT"
  grep -q "ext4-reset" "$OUT"
  grep -q "\[ext4\] unmounted /mnt" "$OUT"
  if rg -q "mv: rename failed|rmdir: failed /mnt/empty|rm: cannot remove /mnt/new2.txt|mkdir: cannot create /mnt/hi" "$OUT"; then
    echo "[test] FAIL: ext4 mutation flow failed" >&2
    exit 1
  fi
  if rg -q "^mount: failed$" "$OUT"; then
    echo "[test] FAIL: mount/umount flow failed" >&2
    exit 1
  fi
  grep -q "umount: failed" "$OUT"
fi

grep -q "smoke-test" "$OUT"
grep -q "kill: failed" "$OUT"
grep -q "ls: open failed" "$OUT"
grep -q "hello world" "$OUT"
grep -q "foo bar" "$OUT"
grep -q "hi" "$OUT"
grep -q "one two" "$OUT"
grep -q "append-one" "$OUT"
grep -q "append-two" "$OUT"
grep -q "mkfs.ext4: formatted" "$OUT"
grep -q "fsck.ext4: clean" "$OUT"
grep -q "mkfst" "$OUT"
grep -q "slept-ok" "$OUT"
grep -q "line-6" "$OUT"
if rg -q "fork failed|\[panic\]|no runnable" "$OUT"; then
  echo "[test] FAIL: runtime error detected" >&2
  exit 1
fi

OUT_AHCI="$(mktemp)"
DISK_AHCI1="$(mktemp)"
DISK_AHCI2="$(mktemp)"
truncate -s 64M "$DISK_AHCI1"
truncate -s 64M "$DISK_AHCI2"

# MBR: one Linux partition at LBA 2048, size 32768 sectors.
printf '\x00\x00\x00\x00\x83\x00\x00\x00\x00\x08\x00\x00\x00\x80\x00\x00' \
  | dd of="$DISK_AHCI1" bs=1 seek=446 conv=notrunc status=none
printf '\x55\xAA' | dd of="$DISK_AHCI1" bs=1 seek=510 conv=notrunc status=none

(
  emit() {
    echo "$1"
    sleep "$STEP"
  }

  sleep 0.8
  emit 'ls /dev'
  emit 'exit'
  sleep 0.3
) | timeout 18 qemu-system-aarch64 \
      -machine virt,virtualization=on,gic-version=2 \
      -cpu cortex-a53 \
      -m 256M \
      -smp 1 \
      -nographic \
      -monitor none \
      -drive if=none,file="$DISK_AHCI1",format=raw,id=sd0 \
      -drive if=none,file="$DISK_AHCI2",format=raw,id=sd1 \
      -device ich9-ahci,id=ahci0 \
      -device ide-hd,drive=sd0,bus=ahci0.0 \
      -device ide-hd,drive=sd1,bus=ahci0.1 \
      -device loader,file=build/kernel.elf,cpu-num=0 >"$OUT_AHCI" 2>&1 || true

cat "$OUT_AHCI"
grep -q "\[ahci\] ready" "$OUT_AHCI"
grep -q "mode=irq" "$OUT_AHCI"
grep -q "sda" "$OUT_AHCI"
grep -q "sdb" "$OUT_AHCI"
grep -q "sda1" "$OUT_AHCI"
if rg -q "^vda$" "$OUT_AHCI"; then
  echo "[test] FAIL: /dev/vda should not exist without virtio disk" >&2
  exit 1
fi
if rg -q "fork failed|\[panic\]|no runnable" "$OUT_AHCI"; then
  echo "[test] FAIL: AHCI runtime error detected" >&2
  exit 1
fi

echo "[test] PASS"
