#include "virtio_blk.h"
#include "config.h"
#include "gic.h"
#include "string.h"
#include "uart.h"
#include "print.h"

#define VIRTIO_MMIO_MAGIC        0x000
#define VIRTIO_MMIO_VERSION      0x004
#define VIRTIO_MMIO_DEVICE_ID    0x008
#define VIRTIO_MMIO_VENDOR_ID    0x00c
#define VIRTIO_MMIO_DEV_FEAT     0x010
#define VIRTIO_MMIO_DEV_SEL      0x014
#define VIRTIO_MMIO_DRV_FEAT     0x020
#define VIRTIO_MMIO_DRV_SEL      0x024
#define VIRTIO_MMIO_GUEST_PGSZ   0x028
#define VIRTIO_MMIO_Q_SEL        0x030
#define VIRTIO_MMIO_Q_NUM_MAX    0x034
#define VIRTIO_MMIO_Q_NUM        0x038
#define VIRTIO_MMIO_Q_ALIGN      0x03c
#define VIRTIO_MMIO_Q_PFN        0x040
#define VIRTIO_MMIO_Q_READY      0x044
#define VIRTIO_MMIO_Q_NOTIFY     0x050
#define VIRTIO_MMIO_INT_STAT     0x060
#define VIRTIO_MMIO_INT_ACK      0x064
#define VIRTIO_MMIO_STATUS       0x070
#define VIRTIO_MMIO_Q_DESC_LO    0x080
#define VIRTIO_MMIO_Q_DESC_HI    0x084
#define VIRTIO_MMIO_Q_DRV_LO     0x090
#define VIRTIO_MMIO_Q_DRV_HI     0x094
#define VIRTIO_MMIO_Q_DEV_LO     0x0a0
#define VIRTIO_MMIO_Q_DEV_HI     0x0a4
#define VIRTIO_MMIO_CONFIG       0x100

#define VIRTIO_MAGIC_VALUE       0x74726976U
#define VIRTIO_VERSION_LEGACY    1U
#define VIRTIO_VERSION_MODERN    2U
#define VIRTIO_DEVICE_BLOCK      2U

#define VIRTIO_MMIO_INT_VRING    (1U << 0)
#define VIRTIO_MMIO_INT_CONFIG   (1U << 1)

#define VIRTIO_STATUS_ACK        (1U << 0)
#define VIRTIO_STATUS_DRIVER     (1U << 1)
#define VIRTIO_STATUS_DRIVEROK   (1U << 2)
#define VIRTIO_STATUS_FEATOK     (1U << 3)
#define VIRTIO_STATUS_FAILED     (1U << 7)

#define VIRTIO_F_VERSION_1       (1U << 0) /* high 32-bit word bit 0 == feature bit 32 */
#define VIRTIO_BLK_F_BLK_SIZE    (1U << 6) /* low 32-bit word */
#define VIRTIO_BLK_F_FLUSH       (1U << 9)
#define VIRTIO_BLK_F_MQ          (1U << 12)

#define VIRTQ_DESC_F_NEXT        1U
#define VIRTQ_DESC_F_WRITE       2U

#define VIRTQ_NUM                8U
#define VIRTQ_ALIGN              PAGE_SIZE

#define VIRTIO_BLK_T_IN          0U
#define VIRTIO_BLK_T_OUT         1U
#define VIRTIO_BLK_T_FLUSH       4U

#define VIRTIO_IO_TIMEOUT_CYCLES_DIV 2U
#define VIRTIO_IO_MAX_RETRIES       3U

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_NUM];
    uint16_t used_event;
} __attribute__((packed)) virtq_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTQ_NUM];
    uint16_t avail_event;
} __attribute__((packed)) virtq_used_t;

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) virtio_blk_req_hdr_t;

typedef enum {
    VIRTIO_IO_OK = 0,
    VIRTIO_IO_NOT_READY,
    VIRTIO_IO_TIMEOUT,
    VIRTIO_IO_BAD_USED,
    VIRTIO_IO_STATUS,
} virtio_io_status_t;

static uint8_t vq_mem[PAGE_SIZE * 2] __attribute__((aligned(PAGE_SIZE)));
static virtq_desc_t *vq_desc;
static virtq_avail_t *vq_avail;
static virtq_used_t *vq_used;

static bool blk_ready;
static uint64_t virtio_base;
static uint32_t transport_version;
static uint32_t virtio_slot;
static uint32_t virtio_irq;
static uint16_t vq_size;
static uint16_t vq_used_idx;
static uint64_t blk_capacity_sectors;
static uint32_t blk_sector_bytes = VIRTIO_BLK_SECTOR_SIZE;
static bool feat_flush;
static bool feat_mq;

static volatile uint32_t vq_irq_events;
static volatile uint32_t cfg_irq_events;

static uint64_t io_timeout_cycles;
static uint32_t io_fail_streak;
static uint32_t io_timeout_count;
static uint32_t io_bad_used_count;
static uint32_t io_status_count;
static uint32_t io_reset_count;

static virtio_blk_req_hdr_t req_hdr;
static uint8_t req_status;
static uint8_t bounce_buf[PAGE_SIZE * 2] __attribute__((aligned(PAGE_SIZE)));

static inline volatile uint32_t *virtio_reg_at(uint64_t base, uint32_t off) {
    return (volatile uint32_t *)(base + (uint64_t)off);
}

static inline volatile uint32_t *virtio_reg(uint32_t off) {
    return virtio_reg_at(virtio_base, off);
}

static inline uint32_t virtio_read32_at(uint64_t base, uint32_t off) {
    return *virtio_reg_at(base, off);
}

static inline uint32_t virtio_read32(uint32_t off) {
    return *virtio_reg(off);
}

static inline void virtio_write32(uint32_t off, uint32_t v) {
    *virtio_reg(off) = v;
}

static inline uint64_t align_up_u64(uint64_t x, uint64_t align) {
    return (x + align - 1UL) & ~(align - 1UL);
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

static void virtio_fail(void) {
    uint32_t st = virtio_read32(VIRTIO_MMIO_STATUS);
    st |= VIRTIO_STATUS_FAILED;
    virtio_write32(VIRTIO_MMIO_STATUS, st);
}

static uint64_t virtio_config_read64(uint32_t off) {
    uint32_t lo = virtio_read32(VIRTIO_MMIO_CONFIG + off);
    uint32_t hi = virtio_read32(VIRTIO_MMIO_CONFIG + off + 4U);
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void virtio_ack_interrupts(uint32_t stat) {
    if ((stat & (VIRTIO_MMIO_INT_VRING | VIRTIO_MMIO_INT_CONFIG)) == 0) {
        return;
    }
    virtio_write32(VIRTIO_MMIO_INT_ACK, stat);
    mb();
    if (stat & VIRTIO_MMIO_INT_VRING) {
        vq_irq_events++;
    }
    if (stat & VIRTIO_MMIO_INT_CONFIG) {
        cfg_irq_events++;
    }
}

static void virtio_poll_interrupts(void) {
    uint32_t stat = virtio_read32(VIRTIO_MMIO_INT_STAT);
    if (stat) {
        virtio_ack_interrupts(stat);
    }
}

static bool virtio_setup_ring_layout(uint16_t qsz) {
    if (qsz == 0 || qsz > VIRTQ_NUM) {
        return false;
    }
    memset(vq_mem, 0, sizeof(vq_mem));

    uint64_t desc_bytes = (uint64_t)qsz * sizeof(virtq_desc_t);
    uint64_t avail_bytes = sizeof(uint16_t) * 2U + (uint64_t)qsz * sizeof(uint16_t) + sizeof(uint16_t);
    uint64_t used_off = align_up_u64(desc_bytes + avail_bytes, VIRTQ_ALIGN);
    uint64_t used_bytes = sizeof(uint16_t) * 2U + (uint64_t)qsz * sizeof(virtq_used_elem_t) + sizeof(uint16_t);
    if (used_off + used_bytes > sizeof(vq_mem)) {
        return false;
    }

    vq_desc = (virtq_desc_t *)(void *)vq_mem;
    vq_avail = (virtq_avail_t *)(void *)(vq_mem + desc_bytes);
    vq_used = (virtq_used_t *)(void *)(vq_mem + used_off);
    return true;
}

static bool virtio_wait_for_completion(uint16_t target_used_idx) {
    uint64_t start = read_cntpct();
    uint64_t deadline = start + io_timeout_cycles;
    uint32_t seen_vq_events = vq_irq_events;

    for (;;) {
        mb();
        if (vq_used->idx == target_used_idx) {
            return true;
        }

        virtio_poll_interrupts();
        mb();
        if (vq_used->idx == target_used_idx) {
            return true;
        }

        if ((int64_t)(read_cntpct() - deadline) > 0) {
            return false;
        }

        if (vq_irq_events == seen_vq_events) {
            __asm__ volatile("yield");
        } else {
            seen_vq_events = vq_irq_events;
        }
    }
}

static int virtio_submit_request(uint32_t type, uint64_t sector,
                                 void *data, uint32_t data_len,
                                 bool data_is_write,
                                 virtio_io_status_t *status_out) {
    if (status_out) {
        *status_out = VIRTIO_IO_OK;
    }
    if (!blk_ready || !vq_desc || !vq_avail || !vq_used || vq_size < 2U) {
        if (status_out) {
            *status_out = VIRTIO_IO_NOT_READY;
        }
        return -1;
    }

    req_hdr.type = type;
    req_hdr.reserved = 0;
    req_hdr.sector = sector;
    req_status = 0xffU;

    uint16_t desc_count = 2;
    vq_desc[0].addr = (uint64_t)(void *)&req_hdr;
    vq_desc[0].len = sizeof(req_hdr);
    vq_desc[0].flags = VIRTQ_DESC_F_NEXT;
    vq_desc[0].next = 1;

    if (data_len != 0U) {
        if (!data || vq_size < 3U) {
            if (status_out) {
                *status_out = VIRTIO_IO_BAD_USED;
            }
            return -1;
        }
        vq_desc[1].addr = (uint64_t)(void *)data;
        vq_desc[1].len = data_len;
        vq_desc[1].flags = VIRTQ_DESC_F_NEXT | (data_is_write ? 0U : VIRTQ_DESC_F_WRITE);
        vq_desc[1].next = 2;

        vq_desc[2].addr = (uint64_t)(void *)&req_status;
        vq_desc[2].len = 1;
        vq_desc[2].flags = VIRTQ_DESC_F_WRITE;
        vq_desc[2].next = 0;
        desc_count = 3;
    } else {
        vq_desc[1].addr = (uint64_t)(void *)&req_status;
        vq_desc[1].len = 1;
        vq_desc[1].flags = VIRTQ_DESC_F_WRITE;
        vq_desc[1].next = 0;
    }

    mb();
    uint16_t aidx = vq_avail->idx;
    vq_avail->ring[aidx % vq_size] = 0;
    mb();
    vq_avail->idx = (uint16_t)(aidx + 1U);
    mb();

    uint16_t target_used = (uint16_t)(vq_used_idx + 1U);
    virtio_write32(VIRTIO_MMIO_Q_NOTIFY, 0);

    if (!virtio_wait_for_completion(target_used)) {
        if (status_out) {
            *status_out = VIRTIO_IO_TIMEOUT;
        }
        return -1;
    }

    mb();
    virtq_used_elem_t ue = vq_used->ring[vq_used_idx % vq_size];
    vq_used_idx = target_used;

    if (ue.id != 0U || req_status != 0U) {
        if (status_out) {
            *status_out = (ue.id != 0U) ? VIRTIO_IO_BAD_USED : VIRTIO_IO_STATUS;
        }
        return -1;
    }

    (void)desc_count;
    return 0;
}

static bool virtio_recover(virtio_io_status_t st) {
    if (st == VIRTIO_IO_TIMEOUT) {
        io_timeout_count++;
    } else if (st == VIRTIO_IO_BAD_USED) {
        io_bad_used_count++;
    } else if (st == VIRTIO_IO_STATUS) {
        io_status_count++;
    }
    io_reset_count++;
    uart_puts("[virtio-blk] I/O recovery reset reason=");
    switch (st) {
        case VIRTIO_IO_TIMEOUT:
            uart_puts("timeout");
            break;
        case VIRTIO_IO_BAD_USED:
            uart_puts("bad-used");
            break;
        case VIRTIO_IO_STATUS:
            uart_puts("status");
            break;
        default:
            uart_puts("other");
            break;
    }
    uart_puts("\n");
    virtio_blk_init();
    return blk_ready;
}

static int virtio_submit_with_recovery(uint32_t type, uint64_t sector,
                                       void *data, uint32_t data_len,
                                       bool data_is_write) {
    virtio_io_status_t st = VIRTIO_IO_OK;
    for (uint32_t attempt = 0; attempt <= VIRTIO_IO_MAX_RETRIES; attempt++) {
        if (virtio_submit_request(type, sector, data, data_len, data_is_write, &st) == 0) {
            io_fail_streak = 0U;
            return 0;
        }
        io_fail_streak++;
        if (st != VIRTIO_IO_TIMEOUT && st != VIRTIO_IO_BAD_USED && st != VIRTIO_IO_STATUS) {
            return -1;
        }
        if (attempt == VIRTIO_IO_MAX_RETRIES || !virtio_recover(st)) {
            return -1;
        }
    }
    return -1;
}

static int virtio_rw_raw(uint64_t lba_512, void *buf, uint32_t len, bool write) {
    return virtio_submit_with_recovery(write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN,
                                       lba_512, buf, len, write);
}

void virtio_blk_init(void) {
    blk_ready = false;
    virtio_base = 0;
    transport_version = 0;
    virtio_slot = 0;
    virtio_irq = 0;
    vq_size = 0;
    vq_used_idx = 0;
    blk_capacity_sectors = 0;
    blk_sector_bytes = VIRTIO_BLK_SECTOR_SIZE;
    feat_flush = false;
    feat_mq = false;
    vq_irq_events = 0;
    cfg_irq_events = 0;
    vq_desc = 0;
    vq_avail = 0;
    vq_used = 0;
    memset(vq_mem, 0, sizeof(vq_mem));

    bool mmio_found = false;
    for (uint32_t i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        uint64_t base = VIRTIO_MMIO0_BASE + (uint64_t)i * VIRTIO_MMIO_STRIDE;
        if (virtio_read32_at(base, VIRTIO_MMIO_MAGIC) != VIRTIO_MAGIC_VALUE) {
            continue;
        }
        mmio_found = true;
        uint32_t ver = virtio_read32_at(base, VIRTIO_MMIO_VERSION);
        if (ver != VIRTIO_VERSION_LEGACY && ver != VIRTIO_VERSION_MODERN) {
            continue;
        }
        if (virtio_read32_at(base, VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_BLOCK) {
            continue;
        }
        virtio_base = base;
        transport_version = ver;
        virtio_slot = i;
        virtio_irq = VIRTIO_MMIO_IRQ_BASE + i;
        break;
    }

    if (!mmio_found) {
        uart_puts("[virtio-blk] no mmio transport\n");
        return;
    }
    if (virtio_base == 0) {
        uart_puts("[virtio-blk] no block device\n");
        return;
    }

    (void)virtio_read32(VIRTIO_MMIO_VENDOR_ID);

    virtio_write32(VIRTIO_MMIO_STATUS, 0);
    mb();
    virtio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    virtio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    virtio_write32(VIRTIO_MMIO_DEV_SEL, 0);
    uint32_t feat_lo = virtio_read32(VIRTIO_MMIO_DEV_FEAT);
    uint32_t feat_hi = 0;
    if (transport_version == VIRTIO_VERSION_MODERN) {
        virtio_write32(VIRTIO_MMIO_DEV_SEL, 1);
        feat_hi = virtio_read32(VIRTIO_MMIO_DEV_FEAT);
        if ((feat_hi & VIRTIO_F_VERSION_1) == 0) {
            uart_puts("[virtio-blk] missing VERSION_1 feature\n");
            virtio_fail();
            return;
        }
    }

    uint32_t drv_feat_lo = 0;
    if (feat_lo & VIRTIO_BLK_F_BLK_SIZE) {
        drv_feat_lo |= VIRTIO_BLK_F_BLK_SIZE;
    }
    if (feat_lo & VIRTIO_BLK_F_FLUSH) {
        drv_feat_lo |= VIRTIO_BLK_F_FLUSH;
    }
    feat_mq = (feat_lo & VIRTIO_BLK_F_MQ) != 0;

    virtio_write32(VIRTIO_MMIO_DRV_SEL, 0);
    virtio_write32(VIRTIO_MMIO_DRV_FEAT, drv_feat_lo);

    uint32_t drv_feat_hi = 0;
    if (transport_version == VIRTIO_VERSION_MODERN) {
        drv_feat_hi |= VIRTIO_F_VERSION_1;
        virtio_write32(VIRTIO_MMIO_DRV_SEL, 1);
        virtio_write32(VIRTIO_MMIO_DRV_FEAT, drv_feat_hi);
    }

    uint32_t st = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;
    if (transport_version == VIRTIO_VERSION_MODERN) {
        st |= VIRTIO_STATUS_FEATOK;
        virtio_write32(VIRTIO_MMIO_STATUS, st);
        if ((virtio_read32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATOK) == 0) {
            uart_puts("[virtio-blk] feature negotiation failed\n");
            virtio_fail();
            return;
        }
    } else {
        virtio_write32(VIRTIO_MMIO_STATUS, st);
    }

    virtio_write32(VIRTIO_MMIO_Q_SEL, 0);
    uint32_t qmax = virtio_read32(VIRTIO_MMIO_Q_NUM_MAX);
    if (qmax < 3U) {
        uart_puts("[virtio-blk] queue too small\n");
        virtio_fail();
        return;
    }

    vq_size = (uint16_t)(qmax < VIRTQ_NUM ? qmax : VIRTQ_NUM);
    if (!virtio_setup_ring_layout(vq_size)) {
        uart_puts("[virtio-blk] queue layout failed\n");
        virtio_fail();
        return;
    }

    virtio_write32(VIRTIO_MMIO_Q_NUM, vq_size);
    if (transport_version == VIRTIO_VERSION_MODERN) {
        uint64_t dpa = (uint64_t)(void *)vq_desc;
        uint64_t apa = (uint64_t)(void *)vq_avail;
        uint64_t upa = (uint64_t)(void *)vq_used;
        virtio_write32(VIRTIO_MMIO_Q_DESC_LO, (uint32_t)dpa);
        virtio_write32(VIRTIO_MMIO_Q_DESC_HI, (uint32_t)(dpa >> 32));
        virtio_write32(VIRTIO_MMIO_Q_DRV_LO, (uint32_t)apa);
        virtio_write32(VIRTIO_MMIO_Q_DRV_HI, (uint32_t)(apa >> 32));
        virtio_write32(VIRTIO_MMIO_Q_DEV_LO, (uint32_t)upa);
        virtio_write32(VIRTIO_MMIO_Q_DEV_HI, (uint32_t)(upa >> 32));
        virtio_write32(VIRTIO_MMIO_Q_READY, 1);
    } else {
        virtio_write32(VIRTIO_MMIO_GUEST_PGSZ, PAGE_SIZE);
        virtio_write32(VIRTIO_MMIO_Q_ALIGN, VIRTQ_ALIGN);
        virtio_write32(VIRTIO_MMIO_Q_PFN, (uint32_t)(((uint64_t)(void *)vq_mem) >> 12));
    }

    blk_capacity_sectors = virtio_config_read64(0);
    if (drv_feat_lo & VIRTIO_BLK_F_BLK_SIZE) {
        uint32_t blk_sz = virtio_read32(VIRTIO_MMIO_CONFIG + 20U);
        if (blk_sz != 0U) {
            blk_sector_bytes = blk_sz;
        }
    }

    if (blk_sector_bytes < VIRTIO_BLK_SECTOR_SIZE ||
        (blk_sector_bytes % VIRTIO_BLK_SECTOR_SIZE) != 0 ||
        blk_sector_bytes > sizeof(bounce_buf)) {
        uart_puts("[virtio-blk] unsupported logical block size\n");
        virtio_fail();
        return;
    }

    feat_flush = (drv_feat_lo & VIRTIO_BLK_F_FLUSH) != 0;
    io_timeout_cycles = read_cntfrq() / VIRTIO_IO_TIMEOUT_CYCLES_DIV;
    if (io_timeout_cycles == 0) {
        io_timeout_cycles = 1;
    }
    io_fail_streak = 0U;
    io_timeout_count = 0U;
    io_bad_used_count = 0U;
    io_status_count = 0U;
    io_reset_count = 0U;

    st |= VIRTIO_STATUS_DRIVEROK;
    virtio_write32(VIRTIO_MMIO_STATUS, st);
    virtio_ack_interrupts(virtio_read32(VIRTIO_MMIO_INT_STAT));

    gic_enable_irq(virtio_irq, 0x90);
    blk_ready = true;

    uart_puts("[virtio-blk] ready v=");
    print_dec((int)transport_version);
    uart_puts(" base=");
    print_hex64(virtio_base);
    uart_puts(" irq=");
    print_dec((int)virtio_irq);
    uart_puts(" sectors=");
    print_hex64(blk_capacity_sectors);
    uart_puts(" sector_bytes=");
    print_dec((int)blk_sector_bytes);
    if (feat_flush) {
        uart_puts(" flush=on");
    } else {
        uart_puts(" flush=off");
    }
    if (feat_mq) {
        uart_puts(" mq=host(singleq)");
    }
    uart_puts("\n");
}

bool virtio_blk_ready(void) {
    return blk_ready;
}

uint64_t virtio_blk_capacity_sectors(void) {
    return blk_capacity_sectors;
}

uint32_t virtio_blk_block_size(void) {
    return blk_sector_bytes;
}

bool virtio_blk_handle_irq(uint32_t intid) {
    if (!blk_ready || intid != virtio_irq || virtio_irq == 0) {
        return false;
    }
    uint32_t stat = virtio_read32(VIRTIO_MMIO_INT_STAT);
    virtio_ack_interrupts(stat);
    return true;
}

int virtio_blk_rw_sector(uint64_t lba, void *buf, bool write) {
    if (!blk_ready || !buf) {
        return -1;
    }
    if (lba >= blk_capacity_sectors) {
        return -1;
    }

    if (blk_sector_bytes == VIRTIO_BLK_SECTOR_SIZE) {
        return virtio_rw_raw(lba, buf, VIRTIO_BLK_SECTOR_SIZE, write);
    }

    uint32_t ratio = blk_sector_bytes / VIRTIO_BLK_SECTOR_SIZE;
    uint64_t block_start = (lba / ratio) * ratio;
    uint32_t in_block = (uint32_t)(lba % ratio);
    uint32_t off = in_block * VIRTIO_BLK_SECTOR_SIZE;

    if (block_start + ratio > blk_capacity_sectors) {
        return -1;
    }

    if (!write) {
        if (virtio_rw_raw(block_start, bounce_buf, blk_sector_bytes, false) != 0) {
            return -1;
        }
        memcpy(buf, bounce_buf + off, VIRTIO_BLK_SECTOR_SIZE);
        return 0;
    }

    if (virtio_rw_raw(block_start, bounce_buf, blk_sector_bytes, false) != 0) {
        return -1;
    }
    memcpy(bounce_buf + off, buf, VIRTIO_BLK_SECTOR_SIZE);
    return virtio_rw_raw(block_start, bounce_buf, blk_sector_bytes, true);
}

int virtio_blk_flush(void) {
    if (!blk_ready) {
        return -1;
    }
    if (!feat_flush) {
        return 0;
    }

    return virtio_submit_with_recovery(VIRTIO_BLK_T_FLUSH, 0, 0, 0, false);
}
