#ifndef FUROS_MMU_H
#define FUROS_MMU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PTE_VALID       (1UL << 0)
#define PTE_TABLE       (1UL << 1)
#define PTE_BLOCK       (0UL << 1)
#define PTE_PAGE        (1UL << 1)
#define PTE_AF          (1UL << 10)
#define PTE_SH_INNER    (3UL << 8)
#define PTE_AP_EL1_RW   (0UL << 6)
#define PTE_AP_EL0_RW   (1UL << 6)
#define PTE_AP_EL1_RO   (2UL << 6)
#define PTE_AP_EL0_RO   (3UL << 6)
#define PTE_ATTR_DEVICE (0UL << 2)
#define PTE_ATTR_NORMAL (1UL << 2)
#define PTE_UXN         (1UL << 54)
#define PTE_PXN         (1UL << 53)
#define PTE_SW_COW      (1UL << 55)
#define PTE_ADDR_MASK   0x0000FFFFFFFFF000UL

void mmu_init(void);
uint64_t mmu_kernel_ttbr1(void);
uint64_t mmu_ttbr0_value(uint64_t ttbr0_phys, uint16_t asid);
void mmu_switch_ttbr0(uint64_t ttbr0_phys, uint16_t asid);
void mmu_tlb_flush_all(void);
void mmu_tlb_flush_asid(uint16_t asid);
void mmu_tlb_flush_va(uint64_t va, uint16_t asid);
uint64_t mmu_make_block_l1(uint64_t pa, uint64_t attrs);
uint64_t mmu_make_block_l2(uint64_t pa, uint64_t attrs);
uint64_t mmu_make_table(uint64_t next_pa);
uint64_t mmu_make_page(uint64_t pa, uint64_t attrs);

#endif
