#!/usr/bin/env bash
set -euo pipefail

JOBS="${JOBS:-4}"
if [[ -z "${NO_BUILD:-}" ]]; then
  make -j"$JOBS"
fi

DISK_PATH="${DISK_PATH:-build/disk.img}"
VIRTIO_ENABLED=0
if [[ -n "${ENABLE_VIRTIO:-}" && -z "${DISABLE_VIRTIO:-}" ]]; then
  VIRTIO_ENABLED=1
fi

if [[ "$VIRTIO_ENABLED" -eq 1 ]]; then
  if [[ ! -f "$DISK_PATH" ]]; then
    mkdir -p "$(dirname "$DISK_PATH")"
    if command -v mke2fs >/dev/null 2>&1; then
      SEED="$(mktemp -d)"
      trap 'rm -rf "$SEED"' EXIT
      mkdir -p "$SEED/docs"
      printf "hello from ext4\n" > "$SEED/hello.txt"
      printf "FuriOS ext4 root\n" > "$SEED/docs/readme.txt"
      ln -sf docs/readme.txt "$SEED/readme-link"
      truncate -s 64M "$DISK_PATH"
      mke2fs -q -F -t ext4 -b 4096 -m 0 \
        -O extent,filetype,^has_journal,^64bit,^metadata_csum,^dir_index \
        -d "$SEED" "$DISK_PATH" 16128
    else
      truncate -s 64M "$DISK_PATH"
    fi
  fi
fi

QEMU_STORAGE_ARGS=()
if [[ "$VIRTIO_ENABLED" -eq 1 ]]; then
  QEMU_STORAGE_ARGS+=(
    -drive if=none,file="$DISK_PATH",format=raw,id=vd0
    -device virtio-blk-device,drive=vd0
  )
fi

SSD_DISK_PATH="${SSD_DISK_PATH:-build/ssd.img}"
SSD_DISK_SIZE="${SSD_DISK_SIZE:-256M}"
AHCI_DISK_PATH="${AHCI_DISK_PATH:-$SSD_DISK_PATH}"
AHCI_DISK2_PATH="${AHCI_DISK2_PATH:-}"

if [[ -z "${DISABLE_AHCI:-}" ]]; then
  if [[ ! -f "$AHCI_DISK_PATH" ]]; then
    mkdir -p "$(dirname "$AHCI_DISK_PATH")"
    truncate -s "$SSD_DISK_SIZE" "$AHCI_DISK_PATH"
  fi
fi

if [[ -z "${DISABLE_AHCI:-}" && ( -n "$AHCI_DISK_PATH" || -n "$AHCI_DISK2_PATH" ) ]]; then
  QEMU_STORAGE_ARGS+=(-device ich9-ahci,id=ahci0)
fi
if [[ -n "$AHCI_DISK_PATH" && -z "${DISABLE_AHCI:-}" ]]; then
  QEMU_STORAGE_ARGS+=(
    -drive if=none,file="$AHCI_DISK_PATH",format=raw,id=sd0
    -device ide-hd,drive=sd0,bus=ahci0.0
  )
fi
if [[ -n "$AHCI_DISK2_PATH" && -z "${DISABLE_AHCI:-}" ]]; then
  if [[ ! -f "$AHCI_DISK2_PATH" ]]; then
    mkdir -p "$(dirname "$AHCI_DISK2_PATH")"
    truncate -s 64M "$AHCI_DISK2_PATH"
  fi
  QEMU_STORAGE_ARGS+=(
    -drive if=none,file="$AHCI_DISK2_PATH",format=raw,id=sd1
    -device ide-hd,drive=sd1,bus=ahci0.1
  )
fi

NVME_COUNT="${NVME_COUNT:-0}"
if [[ -n "${ENABLE_NVME:-}" && "$NVME_COUNT" -eq 0 ]]; then
  NVME_COUNT=1
fi
if [[ -n "${DISABLE_NVME:-}" ]]; then
  NVME_COUNT=0
fi
if [[ ! "$NVME_COUNT" =~ ^[0-9]+$ ]]; then
  echo "invalid NVME_COUNT: $NVME_COUNT" >&2
  exit 1
fi
if [[ "$NVME_COUNT" -gt 8 ]]; then
  NVME_COUNT=8
fi

NVME_DISK_SIZE="${NVME_DISK_SIZE:-256M}"
NVME_DISK_PREFIX="${NVME_DISK_PREFIX:-build/nvme}"
if [[ "$NVME_COUNT" -gt 0 ]]; then
  for ((i=0; i<NVME_COUNT; i++)); do
    NVME_IMG="${NVME_DISK_PREFIX}${i}.img"
    NVME_ADDR="$(printf "0x%x" $((6 + i)))"
    if [[ ! -f "$NVME_IMG" ]]; then
      mkdir -p "$(dirname "$NVME_IMG")"
      truncate -s "$NVME_DISK_SIZE" "$NVME_IMG"
    fi
    QEMU_STORAGE_ARGS+=(
      -drive if=none,file="$NVME_IMG",format=raw,id=nvme${i}
      -device nvme,serial=FuriNVMe${i},drive=nvme${i},bus=pcie.0,addr=${NVME_ADDR}
    )
  done
fi

exec qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=2 \
  -cpu cortex-a53 \
  -m 256M \
  -smp 1 \
  -nographic \
  -monitor none \
  "${QEMU_STORAGE_ARGS[@]}" \
  -device loader,file=build/kernel.elf,cpu-num=0
