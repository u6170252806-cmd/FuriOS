#include "nvme.h"
#include "config.h"
#include "gic.h"
#include "pmm.h"
#include "print.h"
#include "string.h"
#include "uart.h"

#define NVME_CLASS_MASS_STORAGE 0x01U
#define NVME_SUBCLASS_NVM       0x08U

#define NVME_QUEUE_DEPTH_ADMIN  32U
#define NVME_QUEUE_DEPTH_IO     64U
#define NVME_CMD_TIMEOUT_DIV    2U
#define NVME_MAX_BACKOFF_SHIFT  4U
#define NVME_IRQ_FALLBACK_SPINS 256U

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

#define NVME_ADMIN_OPC_CREATE_SQ 0x01U
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
    bool present;
    uint32_t nsid;
    uint32_t lba_size;
    uint64_t lba_count;
    uint64_t sectors_512;
} nvme_ns_t;

typedef struct {
    bool present;
    bool ready;
    bool recovering;
    bool irq_enabled;
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
    uint16_t next_cid;
    uint64_t timeout_cycles;
    uint64_t last_recover_cycle;
    uint8_t fail_streak;
    uint32_t irq_events;
    uint32_t timeout_errors;
    uint32_t cfs_errors;
    uint32_t cid_mismatch_errors;
    uint32_t status_errors;
    uint32_t recoveries;
    uint8_t *io_bounce;
    uint8_t *identify_ctrl;
    uint8_t *identify_ns;
    nvme_queue_t admin_q;
    nvme_queue_t io_q;
    nvme_ns_t ns[NVME_MAX_NAMESPACES];
} nvme_ctrl_t;

typedef enum {
    NVME_SUBMIT_OK = 0,
    NVME_SUBMIT_TIMEOUT = 1,
    NVME_SUBMIT_CFS = 2,
    NVME_SUBMIT_CID_MISMATCH = 3,
    NVME_SUBMIT_STATUS = 4,
} nvme_submit_rc_t;

typedef struct {
    nvme_cpl_t cpl;
    uint16_t status;
    bool dnr;
    uint8_t sct;
    uint8_t sc;
} nvme_submit_info_t;

static nvme_ctrl_t g_ctrls[NVME_MAX_CONTROLLERS];
static uint32_t g_ctrl_count;
static bool g_ready;
static uint64_t g_pci_mmio_next;
static uint64_t g_io_timeout_cycles;

static inline uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + a - 1ULL) & ~(a - 1ULL);
}

static inline uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static inline uint64_t rd_le64(const uint8_t *p) {
    return ((uint64_t)p[0]) |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static inline uint32_t rd_le32(const uint8_t *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
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

static inline uint32_t nvme_read32(const nvme_ctrl_t *c, uint32_t off) {
    return *(volatile uint32_t *)(c->mmio + off);
}

static inline void nvme_write32(const nvme_ctrl_t *c, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(c->mmio + off) = v;
}

static inline uint64_t nvme_read64(const nvme_ctrl_t *c, uint32_t off) {
    uint32_t lo = nvme_read32(c, off);
    uint32_t hi = nvme_read32(c, off + 4U);
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static bool nvme_wait_ready(nvme_ctrl_t *c, bool ready) {
    uint64_t deadline = read_cntpct() + c->timeout_cycles;
    while ((int64_t)(read_cntpct() - deadline) <= 0) {
        uint32_t csts = nvme_read32(c, NVME_REG_CSTS);
        if ((csts & NVME_CSTS_CFS) != 0U) {
            return false;
        }
        if (((csts & NVME_CSTS_RDY) != 0U) == ready) {
            return true;
        }
        __asm__ volatile("yield");
    }
    return false;
}

static bool nvme_ctrl_disable(nvme_ctrl_t *c) {
    uint32_t cc = nvme_read32(c, NVME_REG_CC);
    if ((cc & NVME_CC_EN) != 0U) {
        nvme_write32(c, NVME_REG_CC, cc & ~NVME_CC_EN);
    }
    return nvme_wait_ready(c, false);
}

static bool nvme_ctrl_enable(nvme_ctrl_t *c) {
    uint32_t cc = 0U;
    cc |= ((uint32_t)c->mps << NVME_CC_MPS_SHIFT);
    cc |= (NVME_CC_IOSQES_64 << NVME_CC_IOSQES_SHIFT);
    cc |= (NVME_CC_IOCQES_16 << NVME_CC_IOCQES_SHIFT);
    cc |= NVME_CC_EN;
    nvme_write32(c, NVME_REG_CC, cc);
    return nvme_wait_ready(c, true);
}

static bool nvme_queue_alloc(nvme_ctrl_t *c, nvme_queue_t *q, uint16_t qid, uint16_t depth) {
    uint64_t db_stride_bytes = (uint64_t)(4U << c->db_stride);
    uint64_t sq_bytes = align_up_u64((uint64_t)depth * sizeof(nvme_cmd_t), c->ctrl_page_size);
    uint64_t cq_bytes = align_up_u64((uint64_t)depth * sizeof(nvme_cpl_t), c->ctrl_page_size);
    if (!q || depth < 2U) {
        return false;
    }

    if (!q->sq) {
        q->sq = (nvme_cmd_t *)pmm_alloc((size_t)sq_bytes, c->ctrl_page_size);
    }
    if (!q->cq) {
        q->cq = (nvme_cpl_t *)pmm_alloc((size_t)cq_bytes, c->ctrl_page_size);
    }
    if (!q->sq || !q->cq) {
        return false;
    }

    q->qid = qid;
    q->depth = depth;
    q->sq_tail = 0U;
    q->cq_head = 0U;
    q->cq_phase = 1U;
    q->sq_db = (volatile uint32_t *)(c->mmio + NVME_REG_DBS + ((uint64_t)(2U * qid) * db_stride_bytes));
    q->cq_db = (volatile uint32_t *)(c->mmio + NVME_REG_DBS + ((uint64_t)(2U * qid + 1U) * db_stride_bytes));
    memset(q->sq, 0, (size_t)sq_bytes);
    memset(q->cq, 0, (size_t)cq_bytes);
    return true;
}

static nvme_submit_rc_t nvme_submit_raw(nvme_ctrl_t *c, nvme_queue_t *q, nvme_cmd_t *cmd,
                                        nvme_submit_info_t *info_out) {
    uint16_t cid;
    uint32_t seen_irq = c->irq_events;
    uint32_t irq_wait_spins = 0U;
    uint64_t deadline;

    if (!c || !c->ready || !q || !cmd || q->depth < 2U) {
        return NVME_SUBMIT_STATUS;
    }

    c->next_cid++;
    if (c->next_cid == 0U) {
        c->next_cid = 1U;
    }
    cid = c->next_cid;
    cmd->command_id = cid;

    q->sq[q->sq_tail] = *cmd;
    mb();
    q->sq_tail = (uint16_t)((q->sq_tail + 1U) % q->depth);
    *q->sq_db = q->sq_tail;
    mb();

    deadline = read_cntpct() + c->timeout_cycles;
    while ((int64_t)(read_cntpct() - deadline) <= 0) {
        if ((nvme_read32(c, NVME_REG_CSTS) & NVME_CSTS_CFS) != 0U) {
            return NVME_SUBMIT_CFS;
        }

        if (c->irq_enabled && c->irq_events == seen_irq) {
            if (++irq_wait_spins < NVME_IRQ_FALLBACK_SPINS) {
                __asm__ volatile("yield");
                continue;
            }
            irq_wait_spins = 0U;
        } else {
            seen_irq = c->irq_events;
            irq_wait_spins = 0U;
        }

        nvme_cpl_t cpl = q->cq[q->cq_head];
        if ((uint16_t)(cpl.status & 1U) == (uint16_t)q->cq_phase) {
            uint16_t status = (uint16_t)(cpl.status >> 1);
            q->cq_head = (uint16_t)(q->cq_head + 1U);
            if (q->cq_head == q->depth) {
                q->cq_head = 0U;
                q->cq_phase ^= 1U;
            }
            mb();
            *q->cq_db = q->cq_head;
            mb();

            if (cpl.command_id != cid) {
                return NVME_SUBMIT_CID_MISMATCH;
            }
            if (status == 0U) {
                if (info_out) {
                    memset(info_out, 0, sizeof(*info_out));
                    info_out->cpl = cpl;
                    info_out->status = 0U;
                }
                return NVME_SUBMIT_OK;
            }
            if (info_out) {
                memset(info_out, 0, sizeof(*info_out));
                info_out->cpl = cpl;
                info_out->status = status;
                info_out->dnr = (status & 0x4000U) != 0U;
                info_out->sct = (uint8_t)((status >> 8) & 0x7U);
                info_out->sc = (uint8_t)(status & 0xFFU);
            }
            return NVME_SUBMIT_STATUS;
        }
        __asm__ volatile("yield");
    }
    return NVME_SUBMIT_TIMEOUT;
}

static bool nvme_status_retryable(const nvme_submit_info_t *info) {
    if (!info) {
        return false;
    }
    if (info->dnr) {
        return false;
    }
    if (info->sct == 0x2U) {
        return false;
    }
    if (info->sct == 0x0U && (info->sc == 0x1U || info->sc == 0x2U)) {
        return false;
    }
    return true;
}

static void nvme_note_submit_error(nvme_ctrl_t *c, nvme_submit_rc_t rc,
                                   const nvme_submit_info_t *info) {
    if (!c) {
        return;
    }

    switch (rc) {
        case NVME_SUBMIT_TIMEOUT:
            c->timeout_errors++;
            break;
        case NVME_SUBMIT_CFS:
            c->cfs_errors++;
            break;
        case NVME_SUBMIT_CID_MISMATCH:
            c->cid_mismatch_errors++;
            break;
        case NVME_SUBMIT_STATUS:
            c->status_errors++;
            break;
        default:
            break;
    }

    if ((c->timeout_errors + c->cfs_errors +
         c->cid_mismatch_errors + c->status_errors) % 16U != 1U) {
        return;
    }

    uart_puts("[nvme] submit err ctrl=");
    print_dec((int)(c - &g_ctrls[0]));
    uart_puts(" rc=");
    print_dec((int)rc);
    if (info && rc == NVME_SUBMIT_STATUS) {
        uart_puts(" sct=");
        print_dec((int)info->sct);
        uart_puts(" sc=");
        print_dec((int)info->sc);
        uart_puts(" dnr=");
        print_dec(info->dnr ? 1 : 0);
    }
    uart_puts(" to=");
    print_dec((int)c->timeout_errors);
    uart_puts(" cfs=");
    print_dec((int)c->cfs_errors);
    uart_puts(" cid=");
    print_dec((int)c->cid_mismatch_errors);
    uart_puts(" st=");
    print_dec((int)c->status_errors);
    uart_puts("\n");
}

static bool nvme_create_cq(nvme_ctrl_t *c, uint16_t qid, uint16_t depth) {
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OPC_CREATE_CQ;
    cmd.prp1 = (uint64_t)(uintptr_t)c->io_q.cq;
    cmd.cdw10 = ((uint32_t)(depth - 1U) << 16) | (uint32_t)qid;
    cmd.cdw11 = NVME_QUEUE_PHYS_CONTIG | NVME_CQ_IRQ_ENABLED;
    return nvme_submit_raw(c, &c->admin_q, &cmd, 0) == NVME_SUBMIT_OK;
}

static bool nvme_create_sq(nvme_ctrl_t *c, uint16_t qid, uint16_t cqid, uint16_t depth) {
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OPC_CREATE_SQ;
    cmd.prp1 = (uint64_t)(uintptr_t)c->io_q.sq;
    cmd.cdw10 = ((uint32_t)(depth - 1U) << 16) | (uint32_t)qid;
    cmd.cdw11 = ((uint32_t)cqid << 16) | NVME_QUEUE_PHYS_CONTIG;
    return nvme_submit_raw(c, &c->admin_q, &cmd, 0) == NVME_SUBMIT_OK;
}

static bool nvme_identify(nvme_ctrl_t *c, uint32_t nsid, uint32_t cns, void *buf) {
    nvme_cmd_t cmd;
    memset(buf, 0, c->ctrl_page_size);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OPC_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = (uint64_t)(uintptr_t)buf;
    cmd.cdw10 = cns;
    return nvme_submit_raw(c, &c->admin_q, &cmd, 0) == NVME_SUBMIT_OK;
}

static bool nvme_setup_queues(nvme_ctrl_t *c) {
    uint16_t mqes = (uint16_t)((c->cap & 0xFFFFU) + 1U);
    uint16_t admin_depth = (uint16_t)min_u32(NVME_QUEUE_DEPTH_ADMIN, mqes);
    uint16_t io_depth = (uint16_t)min_u32(NVME_QUEUE_DEPTH_IO, mqes);
    if (admin_depth < 2U || io_depth < 2U) {
        return false;
    }
    if (!nvme_queue_alloc(c, &c->admin_q, 0U, admin_depth)) {
        return false;
    }
    if (!nvme_queue_alloc(c, &c->io_q, 1U, io_depth)) {
        return false;
    }

    c->ready = true;
    if (!nvme_ctrl_disable(c)) {
        c->ready = false;
        return false;
    }

    nvme_write32(c, NVME_REG_INTMS, 0xFFFFFFFFU);
    nvme_write32(c, NVME_REG_AQA, ((uint32_t)(admin_depth - 1U) << 16) | (uint32_t)(admin_depth - 1U));
    nvme_write32(c, NVME_REG_ASQ, (uint32_t)((uint64_t)(uintptr_t)c->admin_q.sq & 0xFFFFFFFFULL));
    nvme_write32(c, NVME_REG_ASQ + 4U, (uint32_t)((uint64_t)(uintptr_t)c->admin_q.sq >> 32));
    nvme_write32(c, NVME_REG_ACQ, (uint32_t)((uint64_t)(uintptr_t)c->admin_q.cq & 0xFFFFFFFFULL));
    nvme_write32(c, NVME_REG_ACQ + 4U, (uint32_t)((uint64_t)(uintptr_t)c->admin_q.cq >> 32));

    if (!nvme_ctrl_enable(c)) {
        c->ready = false;
        return false;
    }

    if (!nvme_create_cq(c, 1U, io_depth) || !nvme_create_sq(c, 1U, 1U, io_depth)) {
        c->ready = false;
        return false;
    }

    c->irq_enabled = c->irq != 0U;
    if (c->irq_enabled) {
        nvme_write32(c, NVME_REG_INTMC, 0x1U);
    }
    c->ready = true;
    return true;
}

static bool nvme_scan_namespaces(nvme_ctrl_t *c) {
    if (!nvme_identify(c, 0U, NVME_IDENTIFY_CNS_CTRL, c->identify_ctrl)) {
        return false;
    }
    memset(c->ns, 0, sizeof(c->ns));

    uint32_t nn = rd_le32(c->identify_ctrl + 516U);
    if (nn > NVME_MAX_NAMESPACES) {
        nn = NVME_MAX_NAMESPACES;
    }

    for (uint32_t nsid = 1U; nsid <= nn; nsid++) {
        if (!nvme_identify(c, nsid, NVME_IDENTIFY_CNS_NS, c->identify_ns)) {
            continue;
        }
        uint8_t *ns = c->identify_ns;
        uint64_t nsze = rd_le64(ns + 0U);
        if (nsze == 0U) {
            continue;
        }

        uint8_t flbas = ns[26] & 0x0FU;
        uint32_t lbaf_off = 128U + (uint32_t)flbas * 4U;
        if (lbaf_off + 2U >= c->ctrl_page_size) {
            continue;
        }
        uint8_t ds = ns[lbaf_off + 2U];
        if (ds >= 31U) {
            continue;
        }
        uint32_t lba_size = 1U << ds;
        if (lba_size < NVME_SECTOR_SIZE_512 || (lba_size % NVME_SECTOR_SIZE_512) != 0U) {
            continue;
        }
        if (lba_size > c->ctrl_page_size) {
            continue;
        }

        uint64_t mul = (uint64_t)lba_size / (uint64_t)NVME_SECTOR_SIZE_512;
        if (nsze > 0U && mul > 0U && nsze > (~0ULL / mul)) {
            continue;
        }

        nvme_ns_t *out = &c->ns[nsid - 1U];
        out->present = true;
        out->nsid = nsid;
        out->lba_size = lba_size;
        out->lba_count = nsze;
        out->sectors_512 = nsze * mul;
    }

    return true;
}

static uint32_t nvme_ns_online_count(const nvme_ctrl_t *c) {
    uint32_t n = 0U;
    if (!c) {
        return 0U;
    }
    for (uint32_t i = 0; i < NVME_MAX_NAMESPACES; i++) {
        if (c->ns[i].present) {
            n++;
        }
    }
    return n;
}

static bool nvme_recover(nvme_ctrl_t *c) {
    if (!c || !c->present || c->recovering) {
        return false;
    }
    c->recovering = true;
    c->ready = false;
    memset(c->ns, 0, sizeof(c->ns));
    bool ok = nvme_setup_queues(c) && nvme_scan_namespaces(c);
    c->ready = ok;
    c->recovering = false;
    if (ok) {
        c->recoveries++;
    }
    if (!ok) {
        uart_puts("[nvme] recovery failed ctrl=");
        print_dec((int)(c - &g_ctrls[0]));
        uart_puts("\n");
    }
    return ok;
}

static void nvme_apply_backoff(nvme_ctrl_t *c) {
    if (!c) {
        return;
    }
    if (c->fail_streak < 0xFFU) {
        c->fail_streak++;
    }
    uint8_t shift = c->fail_streak;
    if (shift > NVME_MAX_BACKOFF_SHIFT) {
        shift = NVME_MAX_BACKOFF_SHIFT;
    }
    uint64_t backoff = g_io_timeout_cycles << shift;
    uint64_t max_backoff = g_io_timeout_cycles << NVME_MAX_BACKOFF_SHIFT;
    if (backoff < g_io_timeout_cycles) {
        backoff = g_io_timeout_cycles;
    }
    if (backoff > max_backoff) {
        backoff = max_backoff;
    }
    c->timeout_cycles = backoff;
}

static void nvme_reset_backoff(nvme_ctrl_t *c) {
    if (!c) {
        return;
    }
    c->fail_streak = 0U;
    c->timeout_cycles = g_io_timeout_cycles;
}

static bool nvme_recover_throttled(nvme_ctrl_t *c) {
    if (!c) {
        return false;
    }
    uint64_t now = read_cntpct();
    uint64_t min_gap = c->timeout_cycles / 4U;
    if (min_gap == 0U) {
        min_gap = 1U;
    }
    while (c->last_recover_cycle != 0U && (now - c->last_recover_cycle) < min_gap) {
        __asm__ volatile("yield");
        now = read_cntpct();
    }
    c->last_recover_cycle = now;
    return nvme_recover(c);
}

static bool nvme_submit_with_retry(nvme_ctrl_t *c, nvme_queue_t *q, nvme_cmd_t *cmd) {
    if (!c || !q || !cmd) {
        return false;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        nvme_submit_info_t info;
        nvme_submit_rc_t rc = nvme_submit_raw(c, q, cmd, &info);
        if (rc == NVME_SUBMIT_OK) {
            nvme_reset_backoff(c);
            return true;
        }
        nvme_note_submit_error(c, rc, &info);

        bool retryable = (rc == NVME_SUBMIT_TIMEOUT ||
                          rc == NVME_SUBMIT_CFS ||
                          rc == NVME_SUBMIT_CID_MISMATCH ||
                          (rc == NVME_SUBMIT_STATUS && nvme_status_retryable(&info)));
        if (!retryable) {
            return false;
        }
        if (attempt == 2) {
            return false;
        }
        nvme_apply_backoff(c);
        if (!nvme_recover_throttled(c)) {
            return false;
        }
    }
    return false;
}

static bool nvme_rw_block(nvme_ctrl_t *c, uint32_t nsid, uint64_t lba, void *buf, bool write) {
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = write ? NVME_NVM_OPC_WRITE : NVME_NVM_OPC_READ;
    cmd.nsid = nsid;
    cmd.prp1 = (uint64_t)(uintptr_t)buf;
    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFFULL);
    cmd.cdw11 = (uint32_t)((lba >> 32) & 0xFFFFFFFFULL);
    cmd.cdw12 = 0U;
    return nvme_submit_with_retry(c, &c->io_q, &cmd);
}

static bool nvme_flush_ns_cmd(nvme_ctrl_t *c, uint32_t nsid) {
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_NVM_OPC_FLUSH;
    cmd.nsid = nsid;
    return nvme_submit_with_retry(c, &c->io_q, &cmd);
}

static bool nvme_probe_one(nvme_ctrl_t *c, uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t cls = pci_read32(bus, dev, fn, 0x08);
    uint8_t subclass = (uint8_t)((cls >> 16) & 0xFFU);
    uint8_t class_code = (uint8_t)((cls >> 24) & 0xFFU);
    uint64_t bar0 = 0U;
    if (class_code != NVME_CLASS_MASS_STORAGE || subclass != NVME_SUBCLASS_NVM) {
        return false;
    }
    if (!pci_assign_bar(bus, dev, fn, 0x10, &bar0) || bar0 == 0U) {
        return false;
    }

    memset(c, 0, sizeof(*c));
    c->present = true;
    c->bus = bus;
    c->dev = dev;
    c->fn = fn;
    c->mmio = (volatile uint8_t *)(uintptr_t)(bar0 & ~0xFFFULL);
    c->irq = pci_irq_for_device(bus, dev, fn);
    c->timeout_cycles = g_io_timeout_cycles;

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    cmd |= 0x0006U;
    cmd &= (uint16_t)~(1U << 10);
    pci_write16(bus, dev, fn, 0x04, cmd);

    c->cap = nvme_read64(c, NVME_REG_CAP);
    c->vs = nvme_read32(c, NVME_REG_VS);
    c->db_stride = (uint32_t)((c->cap >> 32) & 0x0FU);

    uint8_t mps_min = (uint8_t)((c->cap >> 48) & 0x0FU);
    uint8_t mps_max = (uint8_t)((c->cap >> 52) & 0x0FU);
    if (mps_min > mps_max || mps_min > 7U) {
        return false;
    }
    c->mps = mps_min;
    c->ctrl_page_size = (uint32_t)(1U << (12U + c->mps));

    c->identify_ctrl = (uint8_t *)pmm_alloc(c->ctrl_page_size, c->ctrl_page_size);
    c->identify_ns = (uint8_t *)pmm_alloc(c->ctrl_page_size, c->ctrl_page_size);
    c->io_bounce = (uint8_t *)pmm_alloc(c->ctrl_page_size, c->ctrl_page_size);
    if (!c->identify_ctrl || !c->identify_ns || !c->io_bounce) {
        return false;
    }

    if (!nvme_setup_queues(c) || !nvme_scan_namespaces(c)) {
        return false;
    }
    return true;
}

void nvme_init(void) {
    memset(g_ctrls, 0, sizeof(g_ctrls));
    g_ctrl_count = 0U;
    g_ready = false;
    g_pci_mmio_next = PCIE_MMIO_BASE + 0x01000000ULL;
    g_io_timeout_cycles = read_cntfrq() / NVME_CMD_TIMEOUT_DIV;
    if (g_io_timeout_cycles == 0U) {
        g_io_timeout_cycles = 1U;
    }

    for (uint16_t bus = 0; bus < 256 && g_ctrl_count < NVME_MAX_CONTROLLERS; bus++) {
        for (uint8_t dev = 0; dev < 32 && g_ctrl_count < NVME_MAX_CONTROLLERS; dev++) {
            uint8_t max_fn = 1U;
            for (uint8_t fn = 0; fn < max_fn && g_ctrl_count < NVME_MAX_CONTROLLERS; fn++) {
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

                nvme_ctrl_t *c = &g_ctrls[g_ctrl_count];
                if (!nvme_probe_one(c, (uint8_t)bus, dev, fn)) {
                    continue;
                }
                if (c->irq) {
                    gic_enable_irq(c->irq, 0x90U);
                }
                uart_puts("[nvme] ready ctrl=");
                print_dec((int)g_ctrl_count);
                uart_puts(" bar=");
                print_hex64((uint64_t)(uintptr_t)c->mmio);
                uart_puts(" irq=");
                print_dec((int)c->irq);
                uart_puts(" ns=");
                print_dec((int)nvme_ns_online_count(c));
                uart_puts(" mode=");
                uart_puts(c->irq_enabled ? "irq" : "poll");
                uart_puts("\n");
                g_ctrl_count++;
            }
        }
    }

    for (uint32_t i = 0; i < g_ctrl_count; i++) {
        if (nvme_ns_online_count(&g_ctrls[i]) > 0U) {
            g_ready = true;
            break;
        }
    }
    if (!g_ready) {
        uart_puts("[nvme] no controller/namespaces (ecam probe)\n");
    }
}

void nvme_poll(void) {
    for (uint32_t i = 0; i < g_ctrl_count; i++) {
        nvme_ctrl_t *c = &g_ctrls[i];
        if (!c->present || !c->ready) {
            continue;
        }
        if ((nvme_read32(c, NVME_REG_CSTS) & NVME_CSTS_CFS) != 0U) {
            nvme_apply_backoff(c);
            (void)nvme_recover_throttled(c);
        }
    }
}

bool nvme_ready(void) {
    return g_ready;
}

uint32_t nvme_controller_count(void) {
    return g_ctrl_count;
}

bool nvme_controller_ready(uint32_t ctrl) {
    return ctrl < g_ctrl_count && g_ctrls[ctrl].ready;
}

bool nvme_ns_present(uint32_t ctrl, uint32_t nsid) {
    if (ctrl >= g_ctrl_count || nsid == 0U || nsid > NVME_MAX_NAMESPACES) {
        return false;
    }
    nvme_ctrl_t *c = &g_ctrls[ctrl];
    return c->ready && c->ns[nsid - 1U].present && c->ns[nsid - 1U].sectors_512 > 0U;
}

uint64_t nvme_ns_capacity_sectors512(uint32_t ctrl, uint32_t nsid) {
    if (!nvme_ns_present(ctrl, nsid)) {
        return 0U;
    }
    return g_ctrls[ctrl].ns[nsid - 1U].sectors_512;
}

uint32_t nvme_ns_sector_size(uint32_t ctrl, uint32_t nsid) {
    if (!nvme_ns_present(ctrl, nsid)) {
        return 0U;
    }
    return g_ctrls[ctrl].ns[nsid - 1U].lba_size;
}

int nvme_ns_rw(uint32_t ctrl, uint32_t nsid, uint64_t lba512, void *buf, uint32_t count512, bool write) {
    if (!buf || count512 == 0U || !nvme_ns_present(ctrl, nsid)) {
        return -1;
    }
    nvme_ctrl_t *c = &g_ctrls[ctrl];
    nvme_ns_t *ns = &c->ns[nsid - 1U];
    if (lba512 >= ns->sectors_512 || (uint64_t)count512 > (ns->sectors_512 - lba512)) {
        return -1;
    }

    uint8_t *p = (uint8_t *)buf;
    while (count512 > 0U) {
        if (ns->lba_size == NVME_SECTOR_SIZE_512) {
            if (write) {
                memcpy(c->io_bounce, p, NVME_SECTOR_SIZE_512);
            }
            if (!nvme_rw_block(c, ns->nsid, lba512, c->io_bounce, write)) {
                return -1;
            }
            if (!write) {
                memcpy(p, c->io_bounce, NVME_SECTOR_SIZE_512);
            }
        } else {
            uint32_t sectors_per_block = ns->lba_size / NVME_SECTOR_SIZE_512;
            uint64_t ns_lba = lba512 / sectors_per_block;
            uint32_t in_block = (uint32_t)(lba512 % sectors_per_block);
            uint32_t off = in_block * NVME_SECTOR_SIZE_512;

            if (!nvme_rw_block(c, ns->nsid, ns_lba, c->io_bounce, false)) {
                return -1;
            }
            if (write) {
                memcpy(c->io_bounce + off, p, NVME_SECTOR_SIZE_512);
                if (!nvme_rw_block(c, ns->nsid, ns_lba, c->io_bounce, true)) {
                    return -1;
                }
            } else {
                memcpy(p, c->io_bounce + off, NVME_SECTOR_SIZE_512);
            }
        }

        lba512++;
        count512--;
        p += NVME_SECTOR_SIZE_512;
    }
    return 0;
}

int nvme_ns_rw_sector(uint32_t ctrl, uint32_t nsid, uint64_t lba512, void *buf, bool write) {
    return nvme_ns_rw(ctrl, nsid, lba512, buf, 1U, write);
}

int nvme_ns_flush(uint32_t ctrl, uint32_t nsid) {
    if (!nvme_ns_present(ctrl, nsid)) {
        return -1;
    }
    return nvme_flush_ns_cmd(&g_ctrls[ctrl], nsid) ? 0 : -1;
}

bool nvme_handle_irq(uint32_t intid) {
    bool handled = false;
    for (uint32_t i = 0; i < g_ctrl_count; i++) {
        nvme_ctrl_t *c = &g_ctrls[i];
        if (!c->ready || c->irq == 0U || c->irq != intid) {
            continue;
        }
        c->irq_events++;
        mb();
        handled = true;
    }
    return handled;
}
