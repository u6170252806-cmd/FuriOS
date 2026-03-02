# FuriOS

FuriOS is a from-scratch Unix-like operating system targeting ARMv8-A (AArch64), implemented in freestanding C with minimal AArch64 assembly.

It boots in EL2, transitions to EL1 for the kernel, and runs user programs in EL0 with an `SVC`-based syscall ABI.

## Project Goals

- Keep the system understandable end-to-end.
- Build core kernel subsystems without external runtime dependencies.
- Run real userland binaries in EL0 with privilege separation.
- Provide practical storage and filesystem support for iterative OS development.

## Architecture Overview

- Boot path: `EL2 -> EL1 -> EL0`.
- Exception handling: EL1 vector table + sync/IRQ/FIQ/SError handlers.
- Scheduling: round-robin with timer IRQ preemption.
- Syscalls: number in `x8`, args in `x0-x5`, return value in `x0`.
- Process model: `fork`, `exec`, `wait`/`waitpid`, `exit`, `kill`, process groups.

## Memory and VM Features

- 4 KiB pages.
- EL1 MMU enabled with kernel and user mappings.
- COW fork with fault-time private page copy.
- Demand paging for anonymous mappings and stack/heap growth policy.
- `brk/sbrk` user heap control.
- `mmap/munmap/mprotect` for private mappings.
- ASID-aware and targeted TLB invalidation paths.

## Filesystem and Storage

- VFS layer + in-memory filesystem roots.
- devfs nodes under `/dev` (including `null`, `zero`, `tty`).
- ext4 support with active mkfs/fsck integration and journaling work.
- Block cache layer used by ext4 and block-backed paths.
- Storage drivers currently integrated:
  - virtio-blk (`/dev/vda`)
  - AHCI SATA (`/dev/sda`, `/dev/sdb`, ...)
  - NVMe (`/dev/nvme0n1`, `/dev/nvme1n1`, ...)

## Userspace

User binaries run in EL0 and are loaded via the kernel ELF loader.

Included commands (current tree) include:

- `/bin/init`
- `/bin/sh`
- `/bin/ls`
- `/bin/cat`
- `/bin/echo`
- `/bin/clear`
- `/bin/mkdir`
- `/bin/rmdir`
- `/bin/rm`
- `/bin/pwd`
- `/bin/touch`
- `/bin/cp`
- `/bin/mv`
- `/bin/sleep`
- `/bin/kill`
- `/bin/mount`
- `/bin/umount`
- `/bin/ln`
- `/sbin/mkfs.ext4`
- `/sbin/fsck.ext4`

## Build

Requirements:

- `aarch64-elf-gcc`
- `aarch64-elf-objcopy`
- `qemu-system-aarch64`

Build kernel + embedded userspace:

```bash
make -j4
```

## Run

Default run:

```bash
./run.sh
```

Useful run modes:

- AHCI-focused boot:

```bash
DISABLE_VIRTIO=1 ./run.sh
```

- NVMe-only boot:

```bash
DISABLE_VIRTIO=1 DISABLE_AHCI=1 ENABLE_NVME=1 ./run.sh
```

- Multiple NVMe namespaces/controllers in QEMU:

```bash
DISABLE_VIRTIO=1 DISABLE_AHCI=1 NVME_COUNT=4 ./run.sh
```

## Example Workflow (Inside FuriOS Shell)

Format and mount a detected block device:

```sh
mkfs.ext4 /dev/sda
mount /dev/sda /mnt ext4
ls /mnt
```

or with NVMe:

```sh
mkfs.ext4 /dev/nvme0n1
mount /dev/nvme0n1 /mnt ext4
ls /mnt
```

## Repository Layout

- `kernel/` kernel core, VM, traps, scheduler, VFS, filesystems, drivers.
- `user/` EL0 programs, crt, syscall wrappers, small libc-free helpers.
- `include/` shared kernel/user headers and subsystem interfaces.
- `run.sh` QEMU launcher with storage topology options.
- `test.sh` smoke/integration test script.
- `progress.md` implementation log and change history.

## Current Scope

FuriOS is actively developed and is not a production OS yet. It is intended for kernel engineering, architecture exploration, and subsystem bring-up on AArch64.

## License

FuriOS is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.
