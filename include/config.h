#ifndef FUROS_CONFIG_H
#define FUROS_CONFIG_H

#include <stdint.h>

#define PAGE_SIZE              4096UL
#define PAGE_MASK              (~(PAGE_SIZE - 1UL))

#define PHYS_RAM_BASE          0x40000000UL
#define PHYS_RAM_SIZE          0x10000000UL /* 256 MiB */
#define KERNEL_LOAD_ADDR       0x40200000UL
#define KERNEL_HIGH_BASE       0xFFFFFF8000000000UL

#define UART0_BASE             0x09000000UL
#define VIRTIO_MMIO0_BASE      0x0A000000UL
#define VIRTIO_MMIO_STRIDE     0x00000200UL
#define VIRTIO_MMIO_SLOTS      32U
#define VIRTIO_MMIO_IRQ_BASE   48U
#define PCIE_MMIO_BASE         0x10000000UL
#define PCIE_MMIO_SIZE         0x2F000000UL
#define PCIE_ECAM_BASE         0x4010000000UL
#define PCIE_INTX_IRQ_BASE     35U
#define PCIE_INTX_IRQ_COUNT    4U
#define GICD_BASE              0x08000000UL
#define GICC_BASE              0x08010000UL
#define TIMER_IRQ              30U
#define TIMER_HZ               100U

#define USER_VA_BASE           0x00400000UL
#define USER_VA_LIMIT          0x00800000UL
#define USER_STACK_TOP         0x007FF000UL
#define USER_STACK_PAGES       8

#define MAX_TASKS              64
#define MAX_FDS                16
#define MAX_PATH               128
#define MAX_ARGV               16
#define MAX_ARG_LEN            64
#define MAX_USER_PAGES         512
#define MAX_VMAS               64
#define MAX_FILE_CACHE_PAGES   256
#define BLOCK_CACHE_LINES      128

#define TASK_KSTACK_SIZE       (64 * 1024)
#define PMM_OOM_RESERVE_PAGES  64
#define FORK_MIN_HEADROOM_PAGES 16
#define FORK_MAX_COW_RESERVE_PAGES 128

#endif
