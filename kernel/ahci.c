#include "ahci.h"
#include "config.h"
#include "gic.h"
#include "pmm.h"
#include "string.h"
#include "uart.h"
#include "print.h"

#define AHCI_CLASS_MASS_STORAGE 0x01U
#define AHCI_SUBCLASS_SATA      0x06U

#define AHCI_MAX_DISKS          8U
#define AHCI_MAX_PORTS          32U
#define AHCI_PRDT_MAX           32U
#define AHCI_MAX_PRDT_BYTES     (4U * 1024U * 1024U)
#define AHCI_CMD_TIMEOUT_DIV    4U

#define AHCI_PCI_BAR5           0x24U

#ifndef AHCI_FAULT_TIMEOUT_EVERY
#define AHCI_FAULT_TIMEOUT_EVERY 0U
#endif
#ifndef AHCI_FAULT_ERROR_EVERY
#define AHCI_FAULT_ERROR_EVERY 0U
#endif
#ifndef AHCI_FAULT_RESET_STORM_EVERY
#define AHCI_FAULT_RESET_STORM_EVERY 0U
#endif
#ifndef AHCI_FAULT_PARTIAL_EVERY
#define AHCI_FAULT_PARTIAL_EVERY 0U
#endif

#define HBA_GHC_HR              (1U << 0)
#define HBA_GHC_IE              (1U << 1)
#define HBA_GHC_AE              (1U << 31)
#define HBA_PxCMD_ST            (1U << 0)
#define HBA_PxCMD_FRE           (1U << 4)
#define HBA_PxCMD_FR            (1U << 14)
#define HBA_PxCMD_CR            (1U << 15)

#define HBA_PxIS_PCS            (1U << 6)
#define HBA_PxIS_PRCS           (1U << 22)
#define HBA_PxIS_TFES           (1U << 30)
#define HBA_PxIS_LINK_EVENTS    (HBA_PxIS_PCS | HBA_PxIS_PRCS)

#define HBA_PORT_DET_PRESENT    0x3U
#define HBA_PORT_IPM_ACTIVE     0x1U

#define ATA_CMD_IDENTIFY        0xECU
#define ATA_CMD_READ_DMA_EXT    0x25U
#define ATA_CMD_WRITE_DMA_EXT   0x35U
#define ATA_CMD_FLUSH_CACHE_EXT 0xEAU
#define ATA_DEV_BUSY            0x80U
#define ATA_DEV_DRQ             0x08U

typedef volatile struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} hba_port_t;

typedef volatile struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t rsv[0xA0 - 0x2C];
    uint8_t vendor[0x100 - 0xA0];
    hba_port_t ports[AHCI_MAX_PORTS];
} hba_mem_t;

typedef struct {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc;
} __attribute__((packed)) hba_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_entry_t prdt[AHCI_PRDT_MAX];
} __attribute__((packed)) hba_cmd_table_t;

typedef struct {
    bool configured;
    bool present;
    uint8_t port;
    uint64_t sectors;
    uint32_t fail_streak;
    uint64_t timeout_cycles;
    volatile hba_port_t *regs;
    hba_cmd_header_t *clb;
    void *fb;
    hba_cmd_table_t *ctba;
} ahci_disk_t;

typedef enum {
    AHCI_CMD_OK = 0,
    AHCI_CMD_TIMEOUT = 1,
    AHCI_CMD_TFES = 2,
    AHCI_CMD_BUSY = 3,
} ahci_cmd_status_t;

static volatile hba_mem_t *g_hba;
static ahci_disk_t g_disks[AHCI_MAX_DISKS];
static int8_t g_port_to_disk[AHCI_MAX_PORTS];
static uint32_t g_disk_count;
static bool g_ready;
static uint64_t g_pci_mmio_next;
static uint32_t g_irq;
static volatile uint32_t g_irq_events;
static uint64_t g_io_timeout_cycles;
static uint64_t g_cmd_seq;

static inline uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + a - 1ULL) & ~(a - 1ULL);
}

static inline void mb(void) {
    __asm__ volatile("dmb ish" ::: "memory");
}

static inline uint64_t read_cntpct(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntfrq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline uintptr_t pci_cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    return (uintptr_t)(PCIE_ECAM_BASE +
        ((uint64_t)bus << 20) +
        ((uint64_t)dev << 15) +
        ((uint64_t)fn << 12) +
        (uint64_t)(off & 0xFFFU));
}

static inline uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    volatile uint32_t *p = (volatile uint32_t *)pci_cfg_addr(bus, dev, fn, off);
    return *p;
}

static inline void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t v) {
    volatile uint32_t *p = (volatile uint32_t *)pci_cfg_addr(bus, dev, fn, off);
    *p = v;
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    uint32_t v = pci_read32(bus, dev, fn, (uint16_t)(off & ~3U));
    return (uint16_t)((v >> ((off & 2U) * 8U)) & 0xFFFFU);
}

static void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint16_t val) {
    uint16_t base = (uint16_t)(off & ~3U);
    uint32_t shift = (off & 2U) * 8U;
    uint32_t v = pci_read32(bus, dev, fn, base);
    v &= ~(0xFFFFU << shift);
    v |= ((uint32_t)val << shift);
    pci_write32(bus, dev, fn, base, v);
}

static bool pci_assign_bar32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t bar_off,
                             uint32_t *bar_out) {
    uint32_t bar = pci_read32(bus, dev, fn, bar_off);
    if ((bar & 0x1U) != 0U) {
        return false;
    }
    if (((bar >> 1) & 0x3U) == 0x2U) {
        return false;
    }
    if ((bar & ~0xFU) != 0U) {
        *bar_out = bar & ~0xFU;
        return true;
    }

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write16(bus, dev, fn, 0x04, (uint16_t)(cmd & ~0x3U));
    pci_write32(bus, dev, fn, bar_off, 0xFFFFFFFFU);
    uint32_t mask = pci_read32(bus, dev, fn, bar_off);
    pci_write32(bus, dev, fn, bar_off, bar);
    pci_write16(bus, dev, fn, 0x04, cmd);
    if (mask == 0U || mask == 0xFFFFFFFFU) {
        return false;
    }

    uint32_t size_mask = mask & ~0xFU;
    uint32_t size = (~size_mask) + 1U;
    if (size == 0U) {
        return false;
    }
    uint64_t addr = align_up_u64(g_pci_mmio_next, size);
    if (addr + size > (uint64_t)PCIE_MMIO_BASE + (uint64_t)PCIE_MMIO_SIZE) {
        return false;
    }
    g_pci_mmio_next = addr + size;
    pci_write32(bus, dev, fn, bar_off, (uint32_t)addr);
    *bar_out = (uint32_t)addr;
    return true;
}

static uint32_t pci_irq_route(uint8_t dev, uint8_t pin) {
    if (pin < 1U || pin > 4U) {
        return 0U;
    }
    uint32_t swz = ((uint32_t)dev + (uint32_t)(pin - 1U)) % PCIE_INTX_IRQ_COUNT;
    return PCIE_INTX_IRQ_BASE + swz;
}

static uint32_t pci_irq_for_device(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t il = pci_read32(bus, dev, fn, 0x3C);
    uint8_t line = (uint8_t)(il & 0xFFU);
    uint8_t pin = (uint8_t)((il >> 8) & 0xFFU);

    if (line != 0xFFU && line != 0U) {
        return (uint32_t)line;
    }
    return pci_irq_route(dev, pin);
}

static bool ahci_fault_hit(uint32_t every, uint64_t seq) {
    if (every == 0U) {
        return false;
    }
    return (seq % every) == 0U;
}

static bool ahci_configure_port_slot(uint32_t port) {
    volatile hba_port_t *p;
    hba_cmd_header_t *clb;
    void *fb;
    hba_cmd_table_t *ctba;
    uint32_t idx;
    ahci_disk_t *d;

    if (!g_hba || port >= AHCI_MAX_PORTS || g_disk_count >= AHCI_MAX_DISKS) {
        return false;
    }
    if (g_port_to_disk[port] >= 0) {
        return true;
    }

    p = &g_hba->ports[port];
    clb = (hba_cmd_header_t *)pmm_alloc(1024U, 1024U);
    fb = pmm_alloc(256U, 256U);
    ctba = (hba_cmd_table_t *)pmm_alloc(1024U, 128U);
    if (!clb || !fb || !ctba) {
        return false;
    }

    idx = g_disk_count;
    d = &g_disks[idx];
    memset(d, 0, sizeof(*d));
    d->configured = true;
    d->present = false;
    d->port = (uint8_t)port;
    d->regs = p;
    d->fail_streak = 0U;
    d->timeout_cycles = g_io_timeout_cycles;
    d->clb = clb;
    d->fb = fb;
    d->ctba = ctba;

    g_port_to_disk[port] = (int8_t)idx;
    g_disk_count = idx + 1U;
    return true;
}

static bool ahci_port_link_up(volatile hba_port_t *p) {
    uint32_t ssts;
    uint8_t det;
    uint8_t ipm;
    if (!p) {
        return false;
    }
    ssts = p->ssts;
    det = (uint8_t)(ssts & 0x0FU);
    ipm = (uint8_t)((ssts >> 8) & 0x0FU);
    return det == HBA_PORT_DET_PRESENT && ipm != 0U;
}

static bool ahci_port_stop(volatile hba_port_t *p) {
    uint32_t timeout = 1000000U;
    p->cmd &= ~(HBA_PxCMD_ST | HBA_PxCMD_FRE);
    while ((p->cmd & (HBA_PxCMD_FR | HBA_PxCMD_CR)) != 0U && timeout > 0U) {
        timeout--;
    }
    return timeout > 0U;
}

static bool ahci_port_start(volatile hba_port_t *p) {
    uint32_t timeout = 1000000U;
    while ((p->cmd & HBA_PxCMD_CR) != 0U && timeout > 0U) {
        timeout--;
    }
    if (timeout == 0U) {
        return false;
    }
    p->cmd |= HBA_PxCMD_FRE;
    p->cmd |= HBA_PxCMD_ST;
    return true;
}

static bool ahci_port_wait_ready(volatile hba_port_t *p) {
    uint32_t timeout = 2000000U;
    while (timeout > 0U) {
        if (ahci_port_link_up(p)) {
            return true;
        }
        timeout--;
    }
    return false;
}

static bool ahci_port_comreset(volatile hba_port_t *p) {
    uint32_t sctl = p->sctl & ~0x0FU;
    p->sctl = sctl | 0x1U;
    for (volatile uint32_t i = 0; i < 50000U; i++) {
        __asm__ volatile("nop");
    }
    p->sctl = sctl;
    return ahci_port_wait_ready(p);
}

static bool ahci_port_rebase(ahci_disk_t *d) {
    volatile hba_port_t *p;
    if (!d || !d->regs || !d->clb || !d->fb || !d->ctba) {
        return false;
    }
    p = d->regs;
    if (!ahci_port_stop(p)) {
        return false;
    }

    memset(d->clb, 0, sizeof(hba_cmd_header_t) * 32U);
    memset(d->fb, 0, 256U);
    memset(d->ctba, 0, sizeof(*d->ctba));

    p->clb = (uint32_t)(uintptr_t)d->clb;
    p->clbu = (uint32_t)((uint64_t)(uintptr_t)d->clb >> 32);
    p->fb = (uint32_t)(uintptr_t)d->fb;
    p->fbu = (uint32_t)((uint64_t)(uintptr_t)d->fb >> 32);

    d->clb[0].ctba = (uint32_t)(uintptr_t)d->ctba;
    d->clb[0].ctbau = (uint32_t)((uint64_t)(uintptr_t)d->ctba >> 32);

    p->serr = 0xFFFFFFFFU;
    p->is = 0xFFFFFFFFU;
    p->ie = g_irq ? 0xFFFFFFFFU : 0U;

    return ahci_port_start(p);
}

static bool ahci_hba_reset(void) {
    uint32_t timeout = 1000000U;
    if (!g_hba) {
        return false;
    }
    g_hba->ghc |= HBA_GHC_HR;
    while ((g_hba->ghc & HBA_GHC_HR) != 0U && timeout > 0U) {
        timeout--;
    }
    if (timeout == 0U) {
        return false;
    }
    g_hba->ghc |= HBA_GHC_AE;
    if (g_irq) {
        g_hba->ghc |= HBA_GHC_IE;
    }
    g_hba->is = 0xFFFFFFFFU;
    return true;
}

static bool ahci_recover_disk(ahci_disk_t *d) {
    if (!d || !d->configured || !d->regs) {
        return false;
    }

    if (ahci_port_rebase(d) && ahci_port_comreset(d->regs)) {
        return true;
    }

    if (!ahci_hba_reset()) {
        return false;
    }

    for (uint32_t i = 0; i < g_disk_count; i++) {
        if (!g_disks[i].configured) {
            continue;
        }
        if (!ahci_port_rebase(&g_disks[i])) {
            return false;
        }
    }
    return ahci_port_comreset(d->regs);
}

static bool ahci_issue_cmd(ahci_disk_t *d, uint8_t ata_cmd, uint64_t lba, uint16_t count,
                           void *buf, bool write, bool data_phase, ahci_cmd_status_t *status_out) {
    volatile hba_port_t *p;
    hba_cmd_header_t *hdr;
    hba_cmd_table_t *tbl;
    uint64_t deadline;
    uint32_t seen_irq;
    uint64_t seq;

    if (!d || !d->configured || !d->regs || !d->clb || !d->ctba) {
        if (status_out) {
            *status_out = AHCI_CMD_BUSY;
        }
        return false;
    }
    if (data_phase && (!buf || count == 0U)) {
        if (status_out) {
            *status_out = AHCI_CMD_BUSY;
        }
        return false;
    }

    p = d->regs;
    hdr = d->clb;
    tbl = d->ctba;
    seq = ++g_cmd_seq;

    if (ahci_fault_hit(AHCI_FAULT_TIMEOUT_EVERY, seq)) {
        uart_puts("[ahci][fault] injected timeout\n");
        if (status_out) {
            *status_out = AHCI_CMD_TIMEOUT;
        }
        return false;
    }

    uint32_t ready_timeout = 1000000U;
    while ((p->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) != 0U && ready_timeout > 0U) {
        ready_timeout--;
    }
    if (ready_timeout == 0U) {
        if (status_out) {
            *status_out = AHCI_CMD_BUSY;
        }
        return false;
    }

    while ((p->ci & 1U) != 0U) {
        __asm__ volatile("yield");
    }

    memset(hdr, 0, sizeof(hba_cmd_header_t) * 32U);
    memset(tbl, 0, sizeof(*tbl));

    hdr[0].flags = 5U;
    if (write) {
        hdr[0].flags |= (1U << 6);
    }
    hdr[0].ctba = (uint32_t)(uintptr_t)d->ctba;
    hdr[0].ctbau = (uint32_t)((uint64_t)(uintptr_t)d->ctba >> 32);

    if (data_phase) {
        uint32_t rem = (uint32_t)count * 512U;
        uint32_t off = 0U;
        uint32_t prdtl = 0U;

        while (rem > 0U) {
            if (prdtl >= AHCI_PRDT_MAX) {
                return false;
            }
            uint32_t chunk = rem > AHCI_MAX_PRDT_BYTES ? AHCI_MAX_PRDT_BYTES : rem;
            uintptr_t addr = (uintptr_t)((uint8_t *)buf + off);
            tbl->prdt[prdtl].dba = (uint32_t)addr;
            tbl->prdt[prdtl].dbau = (uint32_t)((uint64_t)addr >> 32);
            tbl->prdt[prdtl].dbc = (chunk - 1U) | ((rem == chunk) ? (1U << 31) : 0U);
            rem -= chunk;
            off += chunk;
            prdtl++;
        }
        hdr[0].prdtl = (uint16_t)prdtl;
    }

    uint8_t *cfis = tbl->cfis;
    cfis[0] = 0x27U;
    cfis[1] = 1U << 7;
    cfis[2] = ata_cmd;
    cfis[3] = 0U;
    cfis[4] = (uint8_t)(lba & 0xFFU);
    cfis[5] = (uint8_t)((lba >> 8) & 0xFFU);
    cfis[6] = (uint8_t)((lba >> 16) & 0xFFU);
    cfis[7] = 1U << 6;
    cfis[8] = (uint8_t)((lba >> 24) & 0xFFU);
    cfis[9] = (uint8_t)((lba >> 32) & 0xFFU);
    cfis[10] = (uint8_t)((lba >> 40) & 0xFFU);
    cfis[11] = 0U;
    cfis[12] = (uint8_t)(count & 0xFFU);
    cfis[13] = (uint8_t)((count >> 8) & 0xFFU);

    p->is = 0xFFFFFFFFU;
    mb();
    p->ci = 1U;

    if (ahci_fault_hit(AHCI_FAULT_ERROR_EVERY, seq)) {
        uart_puts("[ahci][fault] injected taskfile error\n");
        p->is |= HBA_PxIS_TFES;
    }

    deadline = read_cntpct() + (d->timeout_cycles ? d->timeout_cycles : g_io_timeout_cycles);
    seen_irq = g_irq_events;

    while ((p->ci & 1U) != 0U) {
        if ((p->is & HBA_PxIS_TFES) != 0U) {
            if (status_out) {
                *status_out = AHCI_CMD_TFES;
            }
            return false;
        }
        if ((int64_t)(read_cntpct() - deadline) > 0) {
            if (status_out) {
                *status_out = AHCI_CMD_TIMEOUT;
            }
            return false;
        }
        if (g_irq && g_irq_events == seen_irq) {
            __asm__ volatile("yield");
        } else {
            seen_irq = g_irq_events;
        }
    }

    if ((p->is & HBA_PxIS_TFES) != 0U) {
        if (status_out) {
            *status_out = AHCI_CMD_TFES;
        }
        return false;
    }
    if (status_out) {
        *status_out = AHCI_CMD_OK;
    }
    return true;
}

static bool ahci_cmd_with_retry(ahci_disk_t *d, uint8_t ata_cmd, uint64_t lba,
                                uint16_t count, void *buf, bool write, bool data_phase) {
    ahci_cmd_status_t st = AHCI_CMD_OK;
    uint64_t max_timeout = g_io_timeout_cycles * 16U;
    if (max_timeout < g_io_timeout_cycles) {
        max_timeout = g_io_timeout_cycles;
    }

    if (ahci_fault_hit(AHCI_FAULT_RESET_STORM_EVERY, g_cmd_seq + 1U)) {
        uart_puts("[ahci][fault] injected reset storm\n");
        (void)ahci_recover_disk(d);
    }
    if (ahci_issue_cmd(d, ata_cmd, lba, count, buf, write, data_phase, &st)) {
        d->fail_streak = 0U;
        d->timeout_cycles = g_io_timeout_cycles;
        return true;
    }

    if (st == AHCI_CMD_TIMEOUT) {
        if (d->fail_streak < 8U) {
            d->fail_streak++;
        }
        uint64_t backoff = g_io_timeout_cycles << (d->fail_streak > 4U ? 4U : d->fail_streak);
        if (backoff < g_io_timeout_cycles) {
            backoff = g_io_timeout_cycles;
        }
        if (backoff > max_timeout) {
            backoff = max_timeout;
        }
        d->timeout_cycles = backoff;
    }

    bool recovered = false;
    if (st == AHCI_CMD_TFES || st == AHCI_CMD_BUSY) {
        recovered = ahci_port_rebase(d);
    } else if (st == AHCI_CMD_TIMEOUT) {
        recovered = ahci_port_rebase(d) && ahci_port_comreset(d->regs);
    }
    if (!recovered) {
        recovered = ahci_recover_disk(d);
    }
    if (!recovered) {
        return false;
    }

    if (!ahci_issue_cmd(d, ata_cmd, lba, count, buf, write, data_phase, &st)) {
        return false;
    }
    d->fail_streak = 0U;
    d->timeout_cycles = g_io_timeout_cycles;
    return true;
}

static uint64_t ahci_identify_capacity(ahci_disk_t *d) {
    uint8_t id[512];
    uint16_t *w = (uint16_t *)id;
    uint64_t sectors;

    if (!ahci_cmd_with_retry(d, ATA_CMD_IDENTIFY, 0, 1, id, false, true)) {
        return 0;
    }

    sectors = (uint64_t)w[100] |
              ((uint64_t)w[101] << 16) |
              ((uint64_t)w[102] << 32) |
              ((uint64_t)w[103] << 48);
    if (sectors == 0U) {
        sectors = (uint64_t)w[60] | ((uint64_t)w[61] << 16);
    }
    return sectors;
}

static void ahci_hotplug_mark(ahci_disk_t *d, bool present, uint64_t sectors, bool verbose) {
    if (!d || !d->configured) {
        return;
    }
    if (d->present == present && d->sectors == sectors) {
        return;
    }
    d->present = present;
    d->sectors = sectors;
    if (verbose) {
        uart_puts("[ahci] port ");
        print_dec((int)d->port);
        uart_puts(present ? " online sectors=" : " offline\n");
        if (present) {
            print_hex64(sectors);
            uart_puts("\n");
        }
    }
}

static void ahci_hotplug_check_disk(ahci_disk_t *d, bool verbose) {
    bool link_up;

    if (!d || !d->configured || !d->regs) {
        return;
    }
    link_up = ahci_port_link_up(d->regs);
    if (d->present && link_up) {
        return;
    }

    if (!link_up) {
        ahci_hotplug_mark(d, false, 0U, verbose);
        return;
    }
    if (!ahci_port_rebase(d)) {
        ahci_hotplug_mark(d, false, 0U, verbose);
        return;
    }
    if (!ahci_port_comreset(d->regs)) {
        ahci_hotplug_mark(d, false, 0U, verbose);
        return;
    }

    uint64_t sectors = ahci_identify_capacity(d);
    if (sectors == 0U) {
        ahci_hotplug_mark(d, false, 0U, verbose);
        return;
    }
    ahci_hotplug_mark(d, true, sectors, verbose);
}

static uint32_t ahci_present_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_disk_count; i++) {
        if (g_disks[i].configured && g_disks[i].present) {
            n++;
        }
    }
    return n;
}

void ahci_poll(void) {
    if (g_hba) {
        uint32_t pi = g_hba->pi;
        for (uint32_t port = 0; port < AHCI_MAX_PORTS; port++) {
            if ((pi & (1U << port)) == 0U) {
                continue;
            }
            if (g_port_to_disk[port] < 0) {
                (void)ahci_configure_port_slot(port);
            }
        }
    }

    for (uint32_t i = 0; i < g_disk_count; i++) {
        ahci_hotplug_check_disk(&g_disks[i], false);
    }
}

void ahci_init(void) {
    g_hba = 0;
    memset(g_disks, 0, sizeof(g_disks));
    memset(g_port_to_disk, -1, sizeof(g_port_to_disk));
    g_disk_count = 0U;
    g_ready = false;
    g_pci_mmio_next = PCIE_MMIO_BASE;
    g_irq = 0U;
    g_irq_events = 0U;
    g_cmd_seq = 0U;

    g_io_timeout_cycles = read_cntfrq() / AHCI_CMD_TIMEOUT_DIV;
    if (g_io_timeout_cycles == 0U) {
        g_io_timeout_cycles = 1U;
    }

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint8_t max_fn = 1;
            for (uint8_t fn = 0; fn < max_fn; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, fn, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFFU);
                if (vendor == 0xFFFFU) {
                    if (fn == 0U) {
                        break;
                    }
                    continue;
                }
                if (fn == 0U) {
                    uint8_t hdr = (uint8_t)((pci_read32((uint8_t)bus, dev, fn, 0x0C) >> 16) & 0xFFU);
                    if ((hdr & 0x80U) != 0U) {
                        max_fn = 8U;
                    }
                }

                uint32_t cls = pci_read32((uint8_t)bus, dev, fn, 0x08);
                uint8_t subclass = (uint8_t)((cls >> 16) & 0xFFU);
                uint8_t class_code = (uint8_t)((cls >> 24) & 0xFFU);
                if (class_code != AHCI_CLASS_MASS_STORAGE || subclass != AHCI_SUBCLASS_SATA) {
                    continue;
                }

                uint32_t abar = 0U;
                if (!pci_assign_bar32((uint8_t)bus, dev, fn, AHCI_PCI_BAR5, &abar) || abar == 0U) {
                    continue;
                }

                uint16_t cmd = pci_read16((uint8_t)bus, dev, fn, 0x04);
                cmd |= 0x0006U;
                cmd &= (uint16_t)~(1U << 10);
                pci_write16((uint8_t)bus, dev, fn, 0x04, cmd);

                g_irq = pci_irq_for_device((uint8_t)bus, dev, fn);

                g_hba = (volatile hba_mem_t *)(uintptr_t)(abar & ~0xFFFU);
                g_hba->ghc |= HBA_GHC_AE;
                if (g_irq) {
                    g_hba->ghc |= HBA_GHC_IE;
                }
                g_hba->is = 0xFFFFFFFFU;

                uint32_t pi = g_hba->pi;
                for (uint32_t port = 0; port < AHCI_MAX_PORTS && g_disk_count < AHCI_MAX_DISKS; port++) {
                    if ((pi & (1U << port)) == 0U) {
                        continue;
                    }
                    if (!ahci_configure_port_slot(port)) {
                        continue;
                    }
                    int8_t idx = g_port_to_disk[port];
                    if (idx >= 0 && (uint32_t)idx < g_disk_count) {
                        ahci_hotplug_check_disk(&g_disks[(uint32_t)idx], false);
                    }
                }

                if (g_disk_count > 0U) {
                    if (g_irq) {
                        gic_enable_irq(g_irq, 0x90);
                    }
                    g_ready = true;
                    uart_puts("[ahci] ready abar=");
                    print_hex64((uint64_t)(uintptr_t)g_hba);
                    uart_puts(" slots=");
                    print_dec((int)g_disk_count);
                    uart_puts(" online=");
                    print_dec((int)ahci_present_count());
                    uart_puts(" irq=");
                    print_dec((int)g_irq);
                    uart_puts(" mode=");
                    uart_puts(g_irq ? "irq" : "poll");
                    uart_puts("\n");
                    return;
                }
            }
        }
    }

    uart_puts("[ahci] no controller/disks (ecam probe)\n");
}

bool ahci_ready(void) {
    return g_ready;
}

uint32_t ahci_disk_count(void) {
    return g_disk_count;
}

bool ahci_disk_present(uint32_t index) {
    if (index >= g_disk_count || !g_disks[index].configured) {
        return false;
    }
    ahci_hotplug_check_disk(&g_disks[index], false);
    return g_disks[index].present;
}

uint64_t ahci_disk_capacity_sectors(uint32_t index) {
    if (!ahci_disk_present(index)) {
        return 0U;
    }
    return g_disks[index].sectors;
}

int ahci_rw(uint32_t index, uint64_t lba, void *buf, uint32_t count, bool write) {
    ahci_disk_t *d;
    uint8_t *p = (uint8_t *)buf;
    bool inject_partial;

    if (index >= g_disk_count || !g_disks[index].configured || !buf) {
        return -1;
    }
    if (count == 0U) {
        return 0;
    }

    d = &g_disks[index];
    ahci_hotplug_check_disk(d, false);
    if (!d->present) {
        return -1;
    }
    if (lba >= d->sectors || (uint64_t)count > (d->sectors - lba)) {
        return -1;
    }

    inject_partial = count > 1U && ahci_fault_hit(AHCI_FAULT_PARTIAL_EVERY, g_cmd_seq + 1U);

    while (count > 0U) {
        uint16_t chunk = count > 65535U ? 65535U : (uint16_t)count;
        if (!ahci_cmd_with_retry(d,
                                 write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT,
                                 lba, chunk, p, write, true)) {
            return -1;
        }
        lba += chunk;
        count -= chunk;
        p += (uint32_t)chunk * 512U;

        if (inject_partial && count > 0U) {
            uart_puts("[ahci][fault] injected partial transfer\n");
            return -1;
        }
    }

    return 0;
}

int ahci_rw_sector(uint32_t index, uint64_t lba, void *buf, bool write) {
    return ahci_rw(index, lba, buf, 1U, write);
}

int ahci_flush(uint32_t index) {
    ahci_disk_t *d;
    if (index >= g_disk_count || !g_disks[index].configured) {
        return -1;
    }
    d = &g_disks[index];
    ahci_hotplug_check_disk(d, false);
    if (!d->present) {
        return -1;
    }
    if (!ahci_cmd_with_retry(d, ATA_CMD_FLUSH_CACHE_EXT, 0, 0, 0, false, false)) {
        return -1;
    }
    return 0;
}

bool ahci_handle_irq(uint32_t intid) {
    if (!g_hba || !g_irq || intid != g_irq) {
        return false;
    }

    uint32_t hba_is = g_hba->is;
    if (hba_is != 0U) {
        for (uint32_t p = 0; p < AHCI_MAX_PORTS; p++) {
            if ((hba_is & (1U << p)) == 0U) {
                continue;
            }
            volatile hba_port_t *pr = &g_hba->ports[p];
            uint32_t pis = pr->is;
            pr->is = pis;

            if ((pis & HBA_PxIS_LINK_EVENTS) != 0U) {
                int8_t idx = g_port_to_disk[p];
                if (idx < 0) {
                    (void)ahci_configure_port_slot(p);
                    idx = g_port_to_disk[p];
                }
                if (idx >= 0 && (uint32_t)idx < g_disk_count) {
                    ahci_hotplug_check_disk(&g_disks[(uint32_t)idx], true);
                }
            }
        }
        g_hba->is = hba_is;
    }

    g_irq_events++;
    mb();
    return true;
}
