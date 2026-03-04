#include "mmu.h"
#include "config.h"
#include "string.h"

#define TT_ENTRIES 512

#define TCR_T0SZ(x)     ((uint64_t)(x) & 0x3FUL)
#define TCR_IRGN0_WBWA  (1UL << 8)
#define TCR_ORGN0_WBWA  (1UL << 10)
#define TCR_SH0_INNER   (3UL << 12)
#define TCR_TG0_4K      (0UL << 14)
#define TCR_T1SZ(x)     (((uint64_t)(x) & 0x3FUL) << 16)
#define TCR_IRGN1_WBWA  (1UL << 24)
#define TCR_ORGN1_WBWA  (1UL << 26)
#define TCR_SH1_INNER   (3UL << 28)
#define TCR_TG1_4K      (2UL << 30)
#define TCR_IPS_40BIT   (2UL << 32)

#define SCTLR_M         (1UL << 0)
#define SCTLR_C         (1UL << 2)
#define SCTLR_I         (1UL << 12)

#define KERNEL_HIGH_BASE 0xFFFFFF8000000000UL

static uint64_t kernel_l1[TT_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t kernel_l2_low[TT_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t kernel_ttbr1_l1[TT_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t active_ttbr0;
static bool active_ttbr0_valid;

#define TTBR_ASID_SHIFT 48
#define TTBR_ASID_MASK  0xFFFFUL
#define TLBI_VA_MASK    0x00000FFFFFFFFFFFUL

uint64_t mmu_make_table(uint64_t next_pa) {
    return (next_pa & PAGE_MASK) | PTE_TABLE | PTE_VALID;
}

uint64_t mmu_make_block_l1(uint64_t pa, uint64_t attrs) {
    return (pa & 0x0000FFFFC0000000UL) | attrs | PTE_VALID | PTE_BLOCK;
}

uint64_t mmu_make_block_l2(uint64_t pa, uint64_t attrs) {
    return (pa & 0x0000FFFFFFE00000UL) | attrs | PTE_VALID | PTE_BLOCK;
}

uint64_t mmu_make_page(uint64_t pa, uint64_t attrs) {
    return (pa & PAGE_MASK) | attrs | PTE_VALID | PTE_PAGE;
}

static inline void dsb_ish(void) {
    __asm__ volatile("dsb ish" ::: "memory");
}

static inline void dsb_ishst(void) {
    __asm__ volatile("dsb ishst" ::: "memory");
}

static inline void isb(void) {
    __asm__ volatile("isb" ::: "memory");
}

static inline void tlbi_vmalle1(void) {
    __asm__ volatile("tlbi vmalle1" ::: "memory");
}

static inline void tlbi_vmalle1is(void) {
    __asm__ volatile("tlbi vmalle1is" ::: "memory");
}

void mmu_tlb_flush_all(void) {
    dsb_ishst();
    tlbi_vmalle1is();
    dsb_ish();
    isb();
}

void mmu_tlb_flush_asid(uint16_t asid) {
    uint64_t operand = ((uint64_t)asid & TTBR_ASID_MASK) << TTBR_ASID_SHIFT;
    dsb_ishst();
    __asm__ volatile("tlbi aside1is, %0" :: "r"(operand) : "memory");
    dsb_ish();
    isb();
}

void mmu_tlb_flush_va(uint64_t va, uint16_t asid) {
    uint64_t operand = (((uint64_t)asid & TTBR_ASID_MASK) << TTBR_ASID_SHIFT) |
                       ((va >> 12) & TLBI_VA_MASK);
    dsb_ishst();
    __asm__ volatile("tlbi vae1is, %0" :: "r"(operand) : "memory");
    dsb_ish();
    isb();
}

uint64_t mmu_ttbr0_value(uint64_t ttbr0_phys, uint16_t asid) {
    uint64_t baddr = ttbr0_phys & 0x0000FFFFFFFFF000UL;
    uint64_t asid_field = ((uint64_t)asid & TTBR_ASID_MASK) << TTBR_ASID_SHIFT;
    return baddr | asid_field;
}

void mmu_switch_ttbr0(uint64_t ttbr0_phys, uint16_t asid) {
    uint64_t ttbr = mmu_ttbr0_value(ttbr0_phys, asid);
    if (active_ttbr0_valid && active_ttbr0 == ttbr) {
        return;
    }
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr));
    isb();
    active_ttbr0 = ttbr;
    active_ttbr0_valid = true;
}

uint64_t mmu_kernel_ttbr1(void) {
    return (uint64_t)kernel_ttbr1_l1;
}

void mmu_init(void) {
    uint64_t normal = PTE_AF | PTE_SH_INNER | PTE_AP_EL1_RW | PTE_ATTR_NORMAL;
    uint64_t device = PTE_AF | PTE_AP_EL1_RW | PTE_ATTR_DEVICE | PTE_UXN | PTE_PXN;

    memset(kernel_l1, 0, sizeof(kernel_l1));
    memset(kernel_l2_low, 0, sizeof(kernel_l2_low));
    memset(kernel_ttbr1_l1, 0, sizeof(kernel_ttbr1_l1));

    /* TTBR0: lower VA space */
    kernel_l1[0] = mmu_make_table((uint64_t)kernel_l2_low);
    kernel_l1[1] = mmu_make_block_l1(0x40000000UL, normal);

    /* MMIO at 0x08000000-0x081FFFFF (GICv2, 2 MiB block). */
    kernel_l2_low[0x08000000UL >> 21] = mmu_make_block_l2(0x08000000UL, device);

    /* MMIO at 0x09000000 (PL011 UART, 2 MiB block). */
    kernel_l2_low[0x09000000UL >> 21] = mmu_make_block_l2(0x09000000UL, device);

    /* MMIO at 0x0A000000 (virtio-mmio transport, 2 MiB block). */
    kernel_l2_low[0x0A000000UL >> 21] = mmu_make_block_l2(0x0A000000UL, device);

    /* PCIe MMIO window for PCI BARs (QEMU virt): 0x10000000..0x3EFFFFFF. */
    {
        uint64_t start = PCIE_MMIO_BASE & ~((uint64_t)(0x200000 - 1));
        uint64_t end = (PCIE_MMIO_BASE + PCIE_MMIO_SIZE + 0x1FFFFFUL) & ~((uint64_t)(0x200000 - 1));
        for (uint64_t pa = start; pa < end; pa += 0x200000ULL) {
            kernel_l2_low[pa >> 21] = mmu_make_block_l2(pa, device);
        }
    }

    /* PCIe ECAM window sits in 0x4000000000..0x403FFFFFFF (1GiB block). */
    kernel_l1[PCIE_ECAM_BASE >> 30] = mmu_make_block_l1(PCIE_ECAM_BASE & 0x0000FFFFC0000000UL, device);

    /* TTBR1: upper VA kernel alias window. */
    kernel_ttbr1_l1[0] = mmu_make_block_l1(0x40000000UL, normal);

    uint64_t mair = 0x000000000000FF00UL;
    uint64_t tcr = TCR_T0SZ(25) | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | TCR_SH0_INNER |
                   TCR_TG0_4K | TCR_T1SZ(25) | TCR_IRGN1_WBWA | TCR_ORGN1_WBWA |
                   TCR_SH1_INNER | TCR_TG1_4K | TCR_IPS_40BIT;

    __asm__ volatile("msr mair_el1, %0" :: "r"(mair));
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)kernel_l1));
    __asm__ volatile("msr ttbr1_el1, %0" :: "r"((uint64_t)kernel_ttbr1_l1));
    active_ttbr0 = (uint64_t)kernel_l1;
    active_ttbr0_valid = true;

    dsb_ish();
    isb();

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    isb();

    (void)KERNEL_HIGH_BASE;
}
