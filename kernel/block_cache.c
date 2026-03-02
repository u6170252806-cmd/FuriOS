#include "block_cache.h"
#include "config.h"
#include "virtio_blk.h"
#include "ahci.h"
#include "nvme.h"
#include "string.h"
#include "uart.h"
#include "print.h"

typedef struct {
    bool valid;
    bool dirty;
    uint64_t lba;
    uint32_t last_use;
    uint8_t data[VIRTIO_BLK_SECTOR_SIZE];
} cache_line_t;

static cache_line_t cache_lines[BLOCK_CACHE_LINES];
static uint32_t use_clock;
static bool cache_ready_flag;
static dev_kind_t active_dev_kind;
static uint64_t active_lba_base;
static uint64_t active_lba_count;
static bool selftest_done;

static const char *dev_name(dev_kind_t kind) {
    switch (kind) {
        case DEV_VDA: return "vda";
        case DEV_SDA: return "sda";
        case DEV_SDB: return "sdb";
        case DEV_SDC: return "sdc";
        case DEV_SDD: return "sdd";
        case DEV_SDE: return "sde";
        case DEV_SDF: return "sdf";
        case DEV_SDG: return "sdg";
        case DEV_SDH: return "sdh";
        case DEV_NVME0N1: return "nvme0n1";
        case DEV_NVME1N1: return "nvme1n1";
        case DEV_NVME2N1: return "nvme2n1";
        case DEV_NVME3N1: return "nvme3n1";
        case DEV_NVME4N1: return "nvme4n1";
        case DEV_NVME5N1: return "nvme5n1";
        case DEV_NVME6N1: return "nvme6n1";
        case DEV_NVME7N1: return "nvme7n1";
        default: return "?";
    }
}

static int sata_index_for_kind(dev_kind_t kind) {
    if (kind < DEV_SDA || kind > DEV_SDH) {
        return -1;
    }
    return (int)(kind - DEV_SDA);
}

static int nvme_index_for_kind(dev_kind_t kind) {
    if (kind < DEV_NVME0N1 || kind > DEV_NVME7N1) {
        return -1;
    }
    return (int)(kind - DEV_NVME0N1);
}

static bool backend_ready(dev_kind_t kind) {
    int sidx = sata_index_for_kind(kind);
    int nidx = nvme_index_for_kind(kind);
    if (kind == DEV_VDA) {
        return virtio_blk_ready();
    }
    if (sidx >= 0) {
        return ahci_disk_present((uint32_t)sidx);
    }
    if (nidx >= 0) {
        return nvme_disk_present((uint32_t)nidx);
    }
    return false;
}

static uint32_t backend_block_size(dev_kind_t kind) {
    int nidx = nvme_index_for_kind(kind);
    if (kind == DEV_VDA) {
        return virtio_blk_block_size();
    }
    if (sata_index_for_kind(kind) >= 0) {
        return VIRTIO_BLK_SECTOR_SIZE;
    }
    if (nidx >= 0) {
        return nvme_disk_sector_size((uint32_t)nidx);
    }
    return 0;
}

static int backend_rw(dev_kind_t kind, uint64_t lba, void *buf, bool write) {
    int sidx = sata_index_for_kind(kind);
    int nidx = nvme_index_for_kind(kind);
    if (kind == DEV_VDA) {
        return virtio_blk_rw_sector(lba, buf, write);
    }
    if (sidx >= 0) {
        return ahci_rw_sector((uint32_t)sidx, lba, buf, write);
    }
    if (nidx >= 0) {
        return nvme_rw_sector((uint32_t)nidx, lba, buf, write);
    }
    return -1;
}

static int backend_flush(dev_kind_t kind) {
    int sidx = sata_index_for_kind(kind);
    int nidx = nvme_index_for_kind(kind);
    if (kind == DEV_VDA) {
        return virtio_blk_flush();
    }
    if (sidx >= 0) {
        return ahci_flush((uint32_t)sidx);
    }
    if (nidx >= 0) {
        return nvme_flush((uint32_t)nidx);
    }
    return -1;
}

static bool cache_selftest(void);

static void cache_touch(cache_line_t *cl) {
    use_clock++;
    if (use_clock == 0) {
        use_clock = 1;
    }
    cl->last_use = use_clock;
}

static int cache_writeback_line(cache_line_t *cl) {
    if (!cl || !cl->valid || !cl->dirty) {
        return 0;
    }
    if (backend_rw(active_dev_kind, active_lba_base + cl->lba, cl->data, true) != 0) {
        return -1;
    }
    cl->dirty = false;
    return 0;
}

static cache_line_t *cache_find(uint64_t lba) {
    for (int i = 0; i < BLOCK_CACHE_LINES; i++) {
        cache_line_t *cl = &cache_lines[i];
        if (cl->valid && cl->lba == lba) {
            cache_touch(cl);
            return cl;
        }
    }
    return 0;
}

static cache_line_t *cache_select_victim(void) {
    cache_line_t *best = 0;
    for (int i = 0; i < BLOCK_CACHE_LINES; i++) {
        cache_line_t *cl = &cache_lines[i];
        if (!cl->valid) {
            return cl;
        }
        if (!best || cl->last_use < best->last_use) {
            best = cl;
        }
    }
    return best;
}

static cache_line_t *cache_get_load(uint64_t lba) {
    cache_line_t *hit = cache_find(lba);
    if (hit) {
        return hit;
    }

    cache_line_t *victim = cache_select_victim();
    if (!victim) {
        return 0;
    }
    if (cache_writeback_line(victim) != 0) {
        return 0;
    }

    if (backend_rw(active_dev_kind, active_lba_base + lba, victim->data, false) != 0) {
        return 0;
    }
    victim->valid = true;
    victim->dirty = false;
    victim->lba = lba;
    cache_touch(victim);
    return victim;
}

void block_cache_init(void) {
    memset(cache_lines, 0, sizeof(cache_lines));
    use_clock = 0;
    cache_ready_flag = false;
    active_dev_kind = DEV_NONE;
    active_lba_base = 0;
    active_lba_count = 0;
    selftest_done = false;
}

int block_cache_attach_inode(const inode_t *inode) {
    dev_kind_t dev_kind;
    uint64_t part_base;
    uint64_t part_count;

    if (!inode || inode->type != INODE_DEV) {
        return -1;
    }
    dev_kind = inode->dev_kind;
    part_base = inode->dev_lba_start;
    part_count = inode->dev_lba_count;

    if (!backend_ready(dev_kind) || part_count == 0U) {
        return -1;
    }
    if (cache_ready_flag && active_dev_kind == dev_kind &&
        active_lba_base == part_base && active_lba_count == part_count) {
        return 0;
    }

    if (cache_ready_flag) {
        (void)block_cache_flush();
    }
    memset(cache_lines, 0, sizeof(cache_lines));
    use_clock = 0;
    cache_ready_flag = true;
    active_dev_kind = dev_kind;
    active_lba_base = part_base;
    active_lba_count = part_count;

    if (!selftest_done) {
        if (!cache_selftest()) {
            cache_ready_flag = false;
            active_dev_kind = DEV_NONE;
            active_lba_base = 0;
            active_lba_count = 0;
            uart_puts("[block-cache] selftest failed, disabled\n");
            return -1;
        }
        selftest_done = true;
    }

    uart_puts("[block-cache] ready dev=");
    uart_puts(dev_name(active_dev_kind));
    uart_puts(" lines=");
    print_dec(BLOCK_CACHE_LINES);
    uart_puts(" dev_block=");
    print_dec((int)backend_block_size(active_dev_kind));
    uart_puts(" lba_base=");
    print_hex64(active_lba_base);
    uart_puts(" lba_count=");
    print_hex64(active_lba_count);
    uart_puts("\n");
    return 0;
}

dev_kind_t block_cache_device(void) {
    return active_dev_kind;
}

bool block_cache_ready(void) {
    return cache_ready_flag && active_dev_kind != DEV_NONE && backend_ready(active_dev_kind) &&
           active_lba_count != 0U;
}

uint64_t block_cache_capacity_sectors(void) {
    if (!block_cache_ready()) {
        return 0;
    }
    return active_lba_count;
}

int block_cache_read(uint64_t lba, void *buf, size_t count) {
    if (!block_cache_ready() || !buf) {
        return -1;
    }
    uint64_t cap = active_lba_count;
    if (count == 0 || lba >= cap || (uint64_t)count > (cap - lba)) {
        return -1;
    }

    uint8_t *out = (uint8_t *)buf;
    for (size_t i = 0; i < count; i++) {
        cache_line_t *cl = cache_get_load(lba + (uint64_t)i);
        if (!cl) {
            return -1;
        }
        memcpy(out + (i * VIRTIO_BLK_SECTOR_SIZE), cl->data, VIRTIO_BLK_SECTOR_SIZE);
    }
    return 0;
}

int block_cache_write(uint64_t lba, const void *buf, size_t count) {
    if (!block_cache_ready() || !buf) {
        return -1;
    }
    uint64_t cap = active_lba_count;
    if (count == 0 || lba >= cap || (uint64_t)count > (cap - lba)) {
        return -1;
    }

    const uint8_t *in = (const uint8_t *)buf;
    for (size_t i = 0; i < count; i++) {
        cache_line_t *cl = cache_get_load(lba + (uint64_t)i);
        if (!cl) {
            return -1;
        }
        memcpy(cl->data, in + (i * VIRTIO_BLK_SECTOR_SIZE), VIRTIO_BLK_SECTOR_SIZE);
        cl->dirty = true;
        cache_touch(cl);
    }
    return 0;
}

int block_cache_flush(void) {
    if (!block_cache_ready()) {
        return -1;
    }
    for (int i = 0; i < BLOCK_CACHE_LINES; i++) {
        if (cache_writeback_line(&cache_lines[i]) != 0) {
            return -1;
        }
    }
    if (backend_flush(active_dev_kind) != 0) {
        return -1;
    }
    return 0;
}

static bool cache_selftest(void) {
    uint64_t cap = active_lba_count;
    if (cap == 0U) {
        return false;
    }
    uint8_t a[VIRTIO_BLK_SECTOR_SIZE];
    uint8_t b[VIRTIO_BLK_SECTOR_SIZE];
    if (block_cache_read(0, a, 1) != 0) {
        return false;
    }
    if (block_cache_read(0, b, 1) != 0) {
        return false;
    }
    if (memcmp(a, b, sizeof(a)) != 0) {
        return false;
    }
    if (block_cache_read(cap, a, 1) == 0) {
        return false;
    }
    uart_puts("[block-cache] selftest ok\n");
    return true;
}
