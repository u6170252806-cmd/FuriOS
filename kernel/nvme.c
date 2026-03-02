#include "nvme.h"
#include "config.h"
#include "gic.h"
#include "pmm.h"
#include "string.h"
#include "uart.h"
#include "print.h"

#define NVME_CLASS_MASS_STORAGE 0x01U
#define NVME_SUBCLASS_NVM       0x08U
#define NVME_PROGIF_NVM         0x02U

#define NVME_MAX_DISKS          8U
#define NVME_QUEUE_DEPTH_ADMIN  32U
#define NVME_QUEUE_DEPTH_IO     64U
#define NVME_CMD_TIMEOUT_DIV    2U

#define NVME_REG_CAP            0x0000U
#define NVME_REG_VS             0x0008U
#define NVME_REG_INTMS          0x000CU
#define NVME_REG_INTMC          0x0010U
#define NVME_REG_CC             0x0014U
#define NVME_REG_CSTS           0x001CU
#define NVME_REG_AQA            0x0024U
#define NVME_REG_ASQ            0x0028U
#define NVME_REG_ACQ            0x0030U
#define NVME_REG_DBS            0x1000U

#define NVME_CC_EN              (1U << 0)
#define NVME_CC_MPS_SHIFT       7U
#define NVME_CC_IOSQES_SHIFT    16U
#define NVME_CC_IOCQES_SHIFT    20U
#define NVME_CC_IOSQES_64       6U
#define NVME_CC_IOCQES_16       4U

#define NVME_CSTS_RDY           (1U << 0)
#define NVME_CSTS_CFS           (1U << 1)

#define NVME_ADMIN_OPC_DELETE_SQ 0x00U
#define NVME_ADMIN_OPC_CREATE_SQ 0x01U
#define NVME_ADMIN_OPC_DELETE_CQ 0x04U
#define NVME_ADMIN_OPC_CREATE_CQ 0x05U
#define NVME_ADMIN_OPC_IDENTIFY  0x06U

#define NVME_NVM_OPC_FLUSH      0x00U
#define NVME_NVM_OPC_WRITE      0x01U
#define NVME_NVM_OPC_READ       0x02U

#define NVME_IDENTIFY_CNS_NS    0x00U
#define NVME_IDENTIFY_CNS_CTRL  0x01U

#define NVME_QUEUE_PHYS_CONTIG  (1U << 0)
#define NVME_CQ_IRQ_ENABLED     (1U << 1)

typedef struct {
    uint8_t opcode;
    uint8_t flags;
    uint16_t command_id;
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_cmd_t;

typedef struct {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;
} __attribute__((packed)) nvme_cpl_t;

typedef struct {
    uint16_t qid;
    uint16_t depth;
    nvme_cmd_t *sq;
    nvme_cpl_t *cq;
    volatile uint32_t *sq_db;
    volatile uint32_t *cq_db;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t cq_phase;
} nvme_queue_t;

typedef struct {
    bool ready;
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
    uint32_t irq;
    volatile uint8_t *mmio;
    uint64_t cap;
    uint32_t vs;
    uint32_t db_stride;
    uint8_t mps;
    uint32_t ctrl_page_size;
    uint32_t sector_size;
    uint64_t sectors_512;
    uint32_t nsid;
    uint16_t next_cid;
    uint8_t *io_bounce;
    nvme_queue_t admin_q;
    nvme_queue_t io_q;
    uint8_t *identify_ctrl;
    uint8_t *identify_ns;
} nvme_disk_t;

static nvme_disk_t g_disks[NVME_MAX_DISKS];
static uint32_t g_disk_count;
static bool g_ready;
static uint64_t g_pci_mmio_next;
static uint64_t g_io_timeout_cycles;

static inline uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + a - 1ULL) & ~(a - 1ULL);
}

static inline uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
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

static bool pci_assign_bar(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t bar_off,
                           uint64_t *bar_out) {
    uint32_t bar_lo = pci_read32(bus, dev, fn, bar_off);
    if ((bar_lo & 0x1U) != 0U) {
        return false;
    }

    uint32_t type = (bar_lo >> 1) & 0x3U;
    bool is64 = type == 0x2U;
    uint32_t bar_hi = is64 ? pci_read32(bus, dev, fn, (uint16_t)(bar_off + 4U)) : 0U;
    uint64_t assigned = ((uint64_t)bar_hi << 32) | (uint64_t)(bar_lo & ~0xFU);
    if (assigned != 0U) {
        *bar_out = assigned;
        return true;
    }

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write16(bus, dev, fn, 0x04, (uint16_t)(cmd & ~0x3U));

    pci_write32(bus, dev, fn, bar_off, 0xFFFFFFFFU);
    uint32_t mask_lo = pci_read32(bus, dev, fn, bar_off);
    uint32_t mask_hi = 0xFFFFFFFFU;
    if (is64) {
        pci_write32(bus, dev, fn, (uint16_t)(bar_off + 4U), 0xFFFFFFFFU);
        mask_hi = pci_read32(bus, dev, fn, (uint16_t)(bar_off + 4U));
    }

    pci_write32(bus, dev, fn, bar_off, bar_lo);
    if (is64) {
        pci_write32(bus, dev, fn, (uint16_t)(bar_off + 4U), bar_hi);
    }
    pci_write16(bus, dev, fn, 0x04, cmd);

    if (mask_lo == 0U || mask_lo == 0xFFFFFFFFU) {
        return false;
    }

    uint64_t mask = ((uint64_t)mask_hi << 32) | (uint64_t)(mask_lo & ~0xFU);
    if (mask == 0U || mask == ~0ULL) {
        return false;
    }

    uint64_t size = (~mask) + 1ULL;
    if (size == 0U) {
        return false;
    }

    uint64_t addr = align_up_u64(g_pci_mmio_next, size);
    if (addr + size > (uint64_t)PCIE_MMIO_BASE + (uint64_t)PCIE_MMIO_SIZE) {
        return false;
    }
    g_pci_mmio_next = addr + size;

    pci_write32(bus, dev, fn, bar_off, (uint32_t)(addr & 0xFFFFFFFFULL));
    if (is64) {
        pci_write32(bus, dev, fn, (uint16_t)(bar_off + 4U), (uint32_t)(addr >> 32));
    }
    *bar_out = addr;
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

static inline uint32_t nvme_read32(const nvme_disk_t *d, uint32_t off) {
    return *(volatile uint32_t *)(d->mmio + off);
}

static inline void nvme_write32(const nvme_disk_t *d, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(d->mmio + off) = v;
}

static inline uint64_t nvme_read64(const nvme_disk_t *d, uint32_t off) {
    uint32_t lo = nvme_read32(d, off);
    uint32_t hi = nvme_read32(d, off + 4U);
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static bool nvme_wait_ready(nvme_disk_t *d, bool ready) {
    uint64_t deadline = read_cntpct() + g_io_timeout_cycles;
    while ((int64_t)(read_cntpct() - deadline) <= 0) {
        uint32_t csts = nvme_read32(d, NVME_REG_CSTS);
        if ((csts & NVME_CSTS_CFS) != 0U) {
            return false;
        }
        bool rdy = (csts & NVME_CSTS_RDY) != 0U;
        if (rdy == ready) {
            return true;
        }
        __asm__ volatile("yield");
    }
    return false;
}

static bool nvme_ctrl_disable(nvme_disk_t *d) {
    uint32_t cc = nvme_read32(d, NVME_REG_CC);
    if ((cc & NVME_CC_EN) != 0U) {
        nvme_write32(d, NVME_REG_CC, cc & ~NVME_CC_EN);
    }
    return nvme_wait_ready(d, false);
}

static bool nvme_ctrl_enable(nvme_disk_t *d) {
    uint32_t cc = 0U;
    cc |= ((uint32_t)d->mps << NVME_CC_MPS_SHIFT);
    cc |= (NVME_CC_IOSQES_64 << NVME_CC_IOSQES_SHIFT);
    cc |= (NVME_CC_IOCQES_16 << NVME_CC_IOCQES_SHIFT);
    cc |= NVME_CC_EN;
    nvme_write32(d, NVME_REG_CC, cc);
    return nvme_wait_ready(d, true);
}

static bool nvme_queue_init(nvme_disk_t *d, nvme_queue_t *q, uint16_t qid, uint16_t depth) {
    uint64_t dbs = (uint64_t)(4U << d->db_stride);
    uint64_t sq_bytes = align_up_u64((uint64_t)depth * sizeof(nvme_cmd_t), d->ctrl_page_size);
    uint64_t cq_bytes = align_up_u64((uint64_t)depth * sizeof(nvme_cpl_t), d->ctrl_page_size);
    if (depth < 2U || !d || !q) {
        return false;
    }

    q->sq = (nvme_cmd_t *)pmm_alloc((size_t)sq_bytes, d->ctrl_page_size);
    q->cq = (nvme_cpl_t *)pmm_alloc((size_t)cq_bytes, d->ctrl_page_size);
    if (!q->sq || !q->cq) {
        return false;
    }

    q->qid = qid;
    q->depth = depth;
    q->sq_tail = 0U;
    q->cq_head = 0U;
    q->cq_phase = 1U;
    q->sq_db = (volatile uint32_t *)(d->mmio + NVME_REG_DBS + ((uint64_t)(2U * qid) * dbs));
    q->cq_db = (volatile uint32_t *)(d->mmio + NVME_REG_DBS + ((uint64_t)(2U * qid + 1U) * dbs));

    memset(q->sq, 0, (size_t)sq_bytes);
    memset(q->cq, 0, (size_t)cq_bytes);
    return true;
}

static bool nvme_cpl_ok(const nvme_cpl_t *cpl) {
    uint16_t status = (uint16_t)((cpl->status >> 1) & 0x7FFU);
    return status == 0U;
}

static bool nvme_submit(nvme_disk_t *d, nvme_queue_t *q, nvme_cmd_t *cmd, nvme_cpl_t *cpl_out) {
    uint16_t cid;
    uint64_t deadline;

    if (!d || !q || !cmd || q->depth < 2U) {
        return false;
    }

    d->next_cid++;
    if (d->next_cid == 0U) {
        d->next_cid = 1U;
    }
    cid = d->next_cid;
    cmd->command_id = cid;

    q->sq[q->sq_tail] = *cmd;
    mb();
    q->sq_tail = (uint16_t)((q->sq_tail + 1U) % q->depth);
    *q->sq_db = q->sq_tail;
    mb();

    deadline = read_cntpct() + g_io_timeout_cycles;
    while ((int64_t)(read_cntpct() - deadline) <= 0) {
        nvme_cpl_t cpl = q->cq[q->cq_head];
        if ((uint16_t)(cpl.status & 1U) == (uint16_t)q->cq_phase) {
            q->cq_head = (uint16_t)(q->cq_head + 1U);
            if (q->cq_head == q->depth) {
                q->cq_head = 0U;
                q->cq_phase ^= 1U;
            }
            mb();
            *q->cq_db = q->cq_head;
            mb();

            if (cpl.command_id != cid) {
                return false;
            }
            if (cpl_out) {
                *cpl_out = cpl;
            }
            return nvme_cpl_ok(&cpl);
        }
        if ((nvme_read32(d, NVME_REG_CSTS) & NVME_CSTS_CFS) != 0U) {
            return false;
        }
        __asm__ volatile("yield");
    }

    return false;
}

static bool nvme_create_cq(nvme_disk_t *d, uint16_t qid, uint16_t depth) {
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OPC_CREATE_CQ;
    cmd.prp1 = (uint64_t)(uintptr_t)d->io_q.cq;
    cmd.cdw10 = ((uint32_t)(depth - 1U) << 16) | (uint32_t)qid;
    cmd.cdw11 = NVME_QUEUE_PHYS_CONTIG | NVME_CQ_IRQ_ENABLED;
    return nvme_submit(d, &d->admin_q, &cmd, 0);
}

static bool nvme_create_sq(nvme_disk_t *d, uint16_t qid, uint16_t cqid, uint16_t depth) {
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OPC_CREATE_SQ;
    cmd.prp1 = (uint64_t)(uintptr_t)d->io_q.sq;
    cmd.cdw10 = ((uint32_t)(depth - 1U) << 16) | (uint32_t)qid;
    cmd.cdw11 = ((uint32_t)cqid << 16) | NVME_QUEUE_PHYS_CONTIG;
    return nvme_submit(d, &d->admin_q, &cmd, 0);
}

static bool nvme_identify(nvme_disk_t *d, uint32_t nsid, uint32_t cns, void *buf) {
    nvme_cmd_t cmd;
    memset(buf, 0, PAGE_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OPC_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = (uint64_t)(uintptr_t)buf;
    cmd.cdw10 = cns;
    return nvme_submit(d, &d->admin_q, &cmd, 0);
}

static bool nvme_setup_queues(nvme_disk_t *d) {
    uint16_t mqes = (uint16_t)((d->cap & 0xFFFFU) + 1U);
    uint16_t admin_depth = (uint16_t)min_u32(NVME_QUEUE_DEPTH_ADMIN, mqes);
    uint16_t io_depth = (uint16_t)min_u32(NVME_QUEUE_DEPTH_IO, mqes);

    if (admin_depth < 2U || io_depth < 2U) {
        return false;
    }
    if (!nvme_queue_init(d, &d->admin_q, 0U, admin_depth)) {
        return false;
    }
    if (!nvme_queue_init(d, &d->io_q, 1U, io_depth)) {
        return false;
    }

    if (!nvme_ctrl_disable(d)) {
        return false;
    }

    nvme_write32(d, NVME_REG_INTMS, 0xFFFFFFFFU);
    nvme_write32(d, NVME_REG_AQA, ((uint32_t)(admin_depth - 1U) << 16) | (uint32_t)(admin_depth - 1U));
    nvme_write32(d, NVME_REG_ASQ, (uint32_t)((uint64_t)(uintptr_t)d->admin_q.sq & 0xFFFFFFFFULL));
    nvme_write32(d, NVME_REG_ASQ + 4U, (uint32_t)((uint64_t)(uintptr_t)d->admin_q.sq >> 32));
    nvme_write32(d, NVME_REG_ACQ, (uint32_t)((uint64_t)(uintptr_t)d->admin_q.cq & 0xFFFFFFFFULL));
    nvme_write32(d, NVME_REG_ACQ + 4U, (uint32_t)((uint64_t)(uintptr_t)d->admin_q.cq >> 32));

    if (!nvme_ctrl_enable(d)) {
        return false;
    }

    if (!nvme_create_cq(d, 1U, io_depth)) {
        return false;
    }
    if (!nvme_create_sq(d, 1U, 1U, io_depth)) {
        return false;
    }
    return true;
}

static bool nvme_read_namespace_info(nvme_disk_t *d) {
    if (!nvme_identify(d, 0U, NVME_IDENTIFY_CNS_CTRL, d->identify_ctrl)) {
        return false;
    }
    if (!nvme_identify(d, 1U, NVME_IDENTIFY_CNS_NS, d->identify_ns)) {
        return false;
    }

    uint8_t *ns = d->identify_ns;
    uint64_t nsze = ((uint64_t)ns[0]) |
                    ((uint64_t)ns[1] << 8) |
                    ((uint64_t)ns[2] << 16) |
                    ((uint64_t)ns[3] << 24) |
                    ((uint64_t)ns[4] << 32) |
                    ((uint64_t)ns[5] << 40) |
                    ((uint64_t)ns[6] << 48) |
                    ((uint64_t)ns[7] << 56);
    uint8_t flbas = ns[26] & 0x0FU;
    uint32_t lbaf_off = 128U + (uint32_t)flbas * 4U;
    uint8_t ds = ns[lbaf_off + 2U];
    uint32_t sec_sz = (ds >= 31U) ? 0U : (1U << ds);

    if (nsze == 0U || sec_sz != NVME_SECTOR_SIZE_512) {
        return false;
    }

    d->sector_size = sec_sz;
    d->sectors_512 = nsze;
    d->nsid = 1U;
    return true;
}

static bool nvme_rw_cmd(nvme_disk_t *d, uint64_t lba, void *buf, uint16_t count, bool write) {
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = write ? NVME_NVM_OPC_WRITE : NVME_NVM_OPC_READ;
    cmd.nsid = d->nsid;
    cmd.prp1 = (uint64_t)(uintptr_t)buf;
    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFFULL);
    cmd.cdw11 = (uint32_t)((lba >> 32) & 0xFFFFFFFFULL);
    cmd.cdw12 = (uint32_t)(count - 1U);
    return nvme_submit(d, &d->io_q, &cmd, 0);
}

static bool nvme_flush_cmd(nvme_disk_t *d) {
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_NVM_OPC_FLUSH;
    cmd.nsid = d->nsid;
    return nvme_submit(d, &d->io_q, &cmd, 0);
}

static bool nvme_probe_one(nvme_disk_t *d, uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t cls = pci_read32(bus, dev, fn, 0x08);
    uint8_t subclass = (uint8_t)((cls >> 16) & 0xFFU);
    uint8_t class_code = (uint8_t)((cls >> 24) & 0xFFU);
    uint64_t bar0 = 0U;

    if (class_code != NVME_CLASS_MASS_STORAGE ||
        subclass != NVME_SUBCLASS_NVM) {
        return false;
    }

    if (!pci_assign_bar(bus, dev, fn, 0x10, &bar0) || bar0 == 0U) {
        return false;
    }

    memset(d, 0, sizeof(*d));
    d->bus = bus;
    d->dev = dev;
    d->fn = fn;
    d->mmio = (volatile uint8_t *)(uintptr_t)(bar0 & ~0xFFFULL);
    d->irq = pci_irq_for_device(bus, dev, fn);

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    cmd |= 0x0006U;
    cmd &= (uint16_t)~(1U << 10);
    pci_write16(bus, dev, fn, 0x04, cmd);

    d->cap = nvme_read64(d, NVME_REG_CAP);
    d->vs = nvme_read32(d, NVME_REG_VS);
    d->db_stride = (uint32_t)((d->cap >> 32) & 0x0FU);
    d->next_cid = 1U;
    uint8_t mps_min = (uint8_t)((d->cap >> 48) & 0x0FU);
    uint8_t mps_max = (uint8_t)((d->cap >> 52) & 0x0FU);
    if (mps_min > mps_max || mps_min > 7U) {
        return false;
    }
    d->mps = mps_min;
    d->ctrl_page_size = (uint32_t)(1U << (12U + d->mps));
    d->identify_ctrl = (uint8_t *)pmm_alloc(d->ctrl_page_size, d->ctrl_page_size);
    d->identify_ns = (uint8_t *)pmm_alloc(d->ctrl_page_size, d->ctrl_page_size);
    d->io_bounce = (uint8_t *)pmm_alloc(d->ctrl_page_size, d->ctrl_page_size);
    if (!d->identify_ctrl || !d->identify_ns || !d->io_bounce) {
        return false;
    }

    if (!nvme_setup_queues(d)) {
        return false;
    }
    if (!nvme_read_namespace_info(d)) {
        return false;
    }

    d->ready = true;
    return true;
}

void nvme_init(void) {
    memset(g_disks, 0, sizeof(g_disks));
    g_disk_count = 0U;
    g_ready = false;
    g_pci_mmio_next = PCIE_MMIO_BASE + 0x01000000ULL;
    g_io_timeout_cycles = read_cntfrq() / NVME_CMD_TIMEOUT_DIV;
    if (g_io_timeout_cycles == 0U) {
        g_io_timeout_cycles = 1U;
    }

    for (uint16_t bus = 0; bus < 256 && g_disk_count < NVME_MAX_DISKS; bus++) {
        for (uint8_t dev = 0; dev < 32 && g_disk_count < NVME_MAX_DISKS; dev++) {
            uint8_t max_fn = 1U;
            for (uint8_t fn = 0; fn < max_fn && g_disk_count < NVME_MAX_DISKS; fn++) {
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

                nvme_disk_t *d = &g_disks[g_disk_count];
                if (!nvme_probe_one(d, (uint8_t)bus, dev, fn)) {
                    continue;
                }
                if (d->irq) {
                    gic_enable_irq(d->irq, 0x90U);
                }
                uart_puts("[nvme] ready ctrl=");
                print_dec((int)g_disk_count);
                uart_puts(" bar=");
                print_hex64((uint64_t)(uintptr_t)d->mmio);
                uart_puts(" irq=");
                print_dec((int)d->irq);
                uart_puts(" sectors=");
                print_hex64(d->sectors_512);
                uart_puts(" sector_bytes=");
                print_dec((int)d->sector_size);
                uart_puts(" mode=poll\n");
                g_disk_count++;
            }
        }
    }

    if (g_disk_count == 0U) {
        uart_puts("[nvme] no controller/namespaces (ecam probe)\n");
        return;
    }
    g_ready = true;
}

void nvme_poll(void) {
    for (uint32_t i = 0; i < g_disk_count; i++) {
        if (!g_disks[i].ready) {
            continue;
        }
        uint32_t csts = nvme_read32(&g_disks[i], NVME_REG_CSTS);
        if ((csts & NVME_CSTS_CFS) != 0U) {
            g_disks[i].ready = false;
        }
    }
}

bool nvme_ready(void) {
    return g_ready;
}

uint32_t nvme_disk_count(void) {
    return g_disk_count;
}

bool nvme_disk_present(uint32_t index) {
    if (index >= g_disk_count) {
        return false;
    }
    return g_disks[index].ready && g_disks[index].sectors_512 != 0U;
}

uint64_t nvme_disk_capacity_sectors(uint32_t index) {
    if (!nvme_disk_present(index)) {
        return 0U;
    }
    return g_disks[index].sectors_512;
}

uint32_t nvme_disk_sector_size(uint32_t index) {
    if (!nvme_disk_present(index)) {
        return 0U;
    }
    return g_disks[index].sector_size;
}

int nvme_rw(uint32_t index, uint64_t lba, void *buf, uint32_t count, bool write) {
    nvme_disk_t *d;
    uint8_t *p = (uint8_t *)buf;
    if (!buf || index >= g_disk_count || count == 0U) {
        return -1;
    }
    d = &g_disks[index];
    if (!d->ready || d->sector_size != NVME_SECTOR_SIZE_512 || d->sectors_512 == 0U) {
        return -1;
    }
    if (lba >= d->sectors_512 || (uint64_t)count > (d->sectors_512 - lba)) {
        return -1;
    }

    while (count > 0U) {
        if (write) {
            memcpy(d->io_bounce, p, NVME_SECTOR_SIZE_512);
        }
        if (!nvme_rw_cmd(d, lba, d->io_bounce, 1U, write)) {
            return -1;
        }
        if (!write) {
            memcpy(p, d->io_bounce, NVME_SECTOR_SIZE_512);
        }
        lba += 1U;
        count -= 1U;
        p += NVME_SECTOR_SIZE_512;
    }
    return 0;
}

int nvme_rw_sector(uint32_t index, uint64_t lba, void *buf, bool write) {
    return nvme_rw(index, lba, buf, 1U, write);
}

int nvme_flush(uint32_t index) {
    nvme_disk_t *d;
    if (index >= g_disk_count) {
        return -1;
    }
    d = &g_disks[index];
    if (!d->ready) {
        return -1;
    }
    if (!nvme_flush_cmd(d)) {
        return -1;
    }
    return 0;
}

bool nvme_handle_irq(uint32_t intid) {
    for (uint32_t i = 0; i < g_disk_count; i++) {
        if (!g_disks[i].ready || g_disks[i].irq == 0U) {
            continue;
        }
        if (g_disks[i].irq == intid) {
            (void)nvme_read32(&g_disks[i], NVME_REG_INTMC);
            return true;
        }
    }
    return false;
}
