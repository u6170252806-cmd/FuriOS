#include "pagecache.h"
#include "config.h"
#include "ext4.h"
#include "block_cache.h"
#include "virtio_blk.h"
#include "pmm.h"
#include "string.h"

typedef struct {
    inode_t *inode;
    uint64_t file_off;
    uint64_t pa;
    uint64_t last_touch;
    bool used;
    bool dirty;
} pagecache_entry_t;

static pagecache_entry_t pagecache[MAX_FILE_CACHE_PAGES];
static uint64_t pagecache_clock;
static uint64_t pagecache_last_tick;

#define PAGECACHE_WRITEBACK_PERIOD 8U
#define PAGECACHE_WRITEBACK_BUDGET 2U
#define PAGECACHE_WRITEBACK_MAX_BUDGET 16U
#define PAGECACHE_DIRTY_HIGH_WATER ((MAX_FILE_CACHE_PAGES * 3U) / 4U)
#define PAGECACHE_DIRTY_CRITICAL (MAX_FILE_CACHE_PAGES - 8U)
#define PAGECACHE_WRITEBACK_FAIL_BACKOFF_TICKS 8U

static uint32_t pagecache_writeback_fail_streak;
static uint64_t pagecache_writeback_blocked_until;

static bool pagecache_writeback_blocked(void);
static void pagecache_note_writeback_result(bool success);

static uint64_t pagecache_touch_tick(void) {
    pagecache_clock++;
    if (pagecache_clock == 0U) {
        pagecache_clock = 1U;
    }
    return pagecache_clock;
}

static bool pagecache_inode_supported(const inode_t *inode) {
    if (!inode) {
        return false;
    }
    if (inode->type == INODE_DEV) {
        return fs_is_block_dev(inode);
    }
    if (inode->type != INODE_FILE) {
        return false;
    }
    return inode->fs_kind == FS_KIND_MEM || inode->fs_kind == FS_KIND_EXT4;
}

static bool pagecache_load_page(inode_t *inode, uint64_t file_off, void *dst_page) {
    if (!inode || !dst_page || !pagecache_inode_supported(inode) ||
        (file_off & (PAGE_SIZE - 1)) != 0) {
        return false;
    }

    memset(dst_page, 0, PAGE_SIZE);
    if (inode->type == INODE_DEV) {
        if (!fs_is_block_dev(inode) || block_cache_attach_inode(inode) != 0) {
            return false;
        }

        const uint64_t sector_size = (uint64_t)VIRTIO_BLK_SECTOR_SIZE;
        const uint64_t sectors_per_page = PAGE_SIZE / sector_size;
        uint64_t dev_sectors = inode->dev_lba_count ? inode->dev_lba_count
                                                    : block_cache_capacity_sectors();
        if (dev_sectors == 0U) {
            return false;
        }
        uint64_t sector_off = file_off / sector_size;
        if (sector_off >= dev_sectors) {
            return true;
        }
        uint64_t rem = dev_sectors - sector_off;
        uint32_t nsec = (uint32_t)(rem < sectors_per_page ? rem : sectors_per_page);
        if (nsec == 0U) {
            return true;
        }
        uint64_t lba = inode->dev_lba_start + sector_off;
        return block_cache_read(lba, dst_page, nsec) == 0;
    }

    if (inode->fs_kind == FS_KIND_MEM) {
        if (!inode->data) {
            return false;
        }
        if (file_off < inode->size) {
            uint64_t rem = (uint64_t)inode->size - file_off;
            size_t n = rem < PAGE_SIZE ? (size_t)rem : (size_t)PAGE_SIZE;
            if (n > 0U) {
                memcpy(dst_page, inode->data + (size_t)file_off, n);
            }
        }
        return true;
    }

    size_t off = (size_t)file_off;
    int n = ext4_read(inode, &off, dst_page, PAGE_SIZE);
    return n >= 0;
}

static bool pagecache_store_page(inode_t *inode, uint64_t file_off, const void *src_page, size_t max_bytes) {
    if (!inode || !src_page || !pagecache_inode_supported(inode) ||
        (file_off & (PAGE_SIZE - 1)) != 0 || max_bytes == 0U) {
        return false;
    }

    size_t n = max_bytes;
    if (n > PAGE_SIZE) {
        n = PAGE_SIZE;
    }

    if (inode->type == INODE_DEV) {
        if (!fs_is_block_dev(inode) || !inode->writable || block_cache_attach_inode(inode) != 0) {
            return false;
        }

        const uint64_t sector_size = (uint64_t)VIRTIO_BLK_SECTOR_SIZE;
        const uint64_t sectors_per_page = PAGE_SIZE / sector_size;
        uint64_t dev_sectors = inode->dev_lba_count ? inode->dev_lba_count
                                                    : block_cache_capacity_sectors();
        uint64_t cap = dev_sectors * sector_size;
        if (file_off >= cap) {
            return false;
        }
        if ((uint64_t)n > cap - file_off) {
            n = (size_t)(cap - file_off);
        }

        uint64_t sector_off = file_off / sector_size;
        uint64_t lba = inode->dev_lba_start + sector_off;
        uint32_t whole = (uint32_t)(n / sector_size);
        if (whole > 0U) {
            if (whole > (uint32_t)sectors_per_page) {
                whole = (uint32_t)sectors_per_page;
            }
            if (block_cache_write(lba, src_page, whole) != 0) {
                return false;
            }
        }

        size_t tail = n - ((size_t)whole * VIRTIO_BLK_SECTOR_SIZE);
        if (tail > 0U) {
            uint8_t sec[VIRTIO_BLK_SECTOR_SIZE];
            uint64_t tail_lba = lba + whole;
            if (block_cache_read(tail_lba, sec, 1U) != 0) {
                return false;
            }
            memcpy(sec, (const uint8_t *)src_page + (size_t)whole * VIRTIO_BLK_SECTOR_SIZE, tail);
            if (block_cache_write(tail_lba, sec, 1U) != 0) {
                return false;
            }
        }
        inode->size = (size_t)cap;
        return true;
    }

    if (inode->fs_kind == FS_KIND_MEM) {
        if (!inode->data || file_off >= inode->capacity) {
            return true;
        }
        size_t cap_rem = inode->capacity - (size_t)file_off;
        if (n > cap_rem) {
            n = cap_rem;
        }
        if (n == 0U) {
            return true;
        }
        memcpy(inode->data + (size_t)file_off, src_page, n);
        size_t end = (size_t)file_off + n;
        if (end > inode->size) {
            inode->size = end;
        }
        return true;
    }

    size_t off = (size_t)file_off;
    if (!ext4_tx_begin()) {
        return false;
    }
    int wr = ext4_write(inode, &off, src_page, n);
    if (wr != (int)n || !ext4_tx_commit()) {
        ext4_tx_abort();
        return false;
    }
    return true;
}

static pagecache_entry_t *pagecache_find(inode_t *inode, uint64_t file_off) {
    for (int i = 0; i < MAX_FILE_CACHE_PAGES; i++) {
        if (!pagecache[i].used) {
            continue;
        }
        if (pagecache[i].inode == inode && pagecache[i].file_off == file_off) {
            return &pagecache[i];
        }
    }
    return 0;
}

static pagecache_entry_t *pagecache_slot(void) {
    for (int i = 0; i < MAX_FILE_CACHE_PAGES; i++) {
        if (!pagecache[i].used) {
            return &pagecache[i];
        }
    }

    pagecache_entry_t *victim = 0;
    for (int i = 0; i < MAX_FILE_CACHE_PAGES; i++) {
        pagecache_entry_t *e = &pagecache[i];
        if (pmm_page_refcount((const void *)(uintptr_t)e->pa) > 1U) {
            continue;
        }
        if (!victim || e->last_touch < victim->last_touch) {
            victim = e;
        }
    }
    if (!victim) {
        return 0;
    }
    if (victim->dirty) {
        size_t max_bytes = PAGE_SIZE;
        if (victim->inode->size <= victim->file_off) {
            victim->dirty = false;
        } else {
            uint64_t rem = (uint64_t)victim->inode->size - victim->file_off;
            if (rem < max_bytes) {
                max_bytes = (size_t)rem;
            }
            if (max_bytes != 0U &&
                !pagecache_store_page(victim->inode, victim->file_off,
                                      (const void *)(uintptr_t)victim->pa, max_bytes)) {
                pagecache_note_writeback_result(false);
                return 0;
            }
            victim->dirty = false;
            pagecache_note_writeback_result(true);
        }
    }
    pmm_free_page((void *)(uintptr_t)victim->pa);
    memset(victim, 0, sizeof(*victim));
    return victim;
}

static bool pagecache_has_free_slot(void) {
    for (int i = 0; i < MAX_FILE_CACHE_PAGES; i++) {
        if (!pagecache[i].used) {
            return true;
        }
    }
    return false;
}

static bool pagecache_entry_writeback(pagecache_entry_t *e) {
    if (!e || !e->used || !e->dirty) {
        return true;
    }
    size_t max_bytes = PAGE_SIZE;
    if (e->inode->size <= e->file_off) {
        e->dirty = false;
        return true;
    }
    uint64_t rem = (uint64_t)e->inode->size - e->file_off;
    if (rem < max_bytes) {
        max_bytes = (size_t)rem;
    }
    if (max_bytes == 0U) {
        e->dirty = false;
        return true;
    }
    if (!pagecache_store_page(e->inode, e->file_off, (const void *)(uintptr_t)e->pa, max_bytes)) {
        return false;
    }
    e->dirty = pmm_page_refcount((const void *)(uintptr_t)e->pa) > 1U;
    e->last_touch = pagecache_touch_tick();
    return true;
}

static bool pagecache_range_overlaps(const pagecache_entry_t *e, uint64_t start_off, uint64_t len) {
    if (!e || !e->used) {
        return false;
    }
    if (len == 0U) {
        return e->file_off >= start_off;
    }

    uint64_t end_off = start_off + len;
    if (end_off < start_off) {
        end_off = ~0ULL;
    }
    uint64_t page_end = e->file_off + PAGE_SIZE;
    if (page_end < e->file_off) {
        page_end = ~0ULL;
    }
    return !(page_end <= start_off || e->file_off >= end_off);
}

static bool pagecache_writeback_blocked(void) {
    return pagecache_writeback_fail_streak != 0U &&
           pagecache_last_tick < pagecache_writeback_blocked_until;
}

static void pagecache_note_writeback_result(bool success) {
    if (success) {
        pagecache_writeback_fail_streak = 0U;
        pagecache_writeback_blocked_until = 0U;
        return;
    }

    if (pagecache_writeback_fail_streak < 255U) {
        pagecache_writeback_fail_streak++;
    }
    uint64_t backoff = (uint64_t)PAGECACHE_WRITEBACK_FAIL_BACKOFF_TICKS *
                       (uint64_t)pagecache_writeback_fail_streak;
    if (backoff < (uint64_t)PAGECACHE_WRITEBACK_FAIL_BACKOFF_TICKS) {
        backoff = ~0ULL;
    }
    pagecache_writeback_blocked_until = pagecache_last_tick + backoff;
    if (pagecache_writeback_blocked_until < pagecache_last_tick) {
        pagecache_writeback_blocked_until = ~0ULL;
    }
}

static int pagecache_flush_match(inode_t *inode, uint64_t start_off, uint64_t len,
                                 bool use_range, uint32_t budget) {
    while (budget > 0U) {
        pagecache_entry_t *victim = 0;
        for (int i = 0; i < MAX_FILE_CACHE_PAGES; i++) {
            pagecache_entry_t *e = &pagecache[i];
            if (!e->used || !e->dirty) {
                continue;
            }
            if (inode && e->inode != inode) {
                continue;
            }
            if (use_range && !pagecache_range_overlaps(e, start_off, len)) {
                continue;
            }
            if (!victim || e->last_touch < victim->last_touch) {
                victim = e;
            }
        }
        if (!victim) {
            break;
        }
        if (!pagecache_entry_writeback(victim)) {
            pagecache_note_writeback_result(false);
            return -1;
        }
        pagecache_note_writeback_result(true);
        budget--;
    }
    return 0;
}

static uint32_t pagecache_dirty_count(void) {
    uint32_t n = 0U;
    for (int i = 0; i < MAX_FILE_CACHE_PAGES; i++) {
        if (pagecache[i].used && pagecache[i].dirty) {
            n++;
        }
    }
    return n;
}

static void pagecache_maybe_writeback_pressure(void) {
    if (pagecache_writeback_blocked()) {
        return;
    }
    uint32_t dirty = pagecache_dirty_count();
    if (dirty < PAGECACHE_DIRTY_HIGH_WATER) {
        return;
    }

    uint32_t budget = PAGECACHE_WRITEBACK_MAX_BUDGET;
    if (dirty >= PAGECACHE_DIRTY_CRITICAL) {
        budget = (uint32_t)MAX_FILE_CACHE_PAGES;
    } else {
        budget += (dirty - PAGECACHE_DIRTY_HIGH_WATER);
        if (budget > (uint32_t)MAX_FILE_CACHE_PAGES) {
            budget = (uint32_t)MAX_FILE_CACHE_PAGES;
        }
    }
    (void)pagecache_flush_match(0, 0U, 0U, false, budget);
}

void pagecache_init(void) {
    memset(pagecache, 0, sizeof(pagecache));
    pagecache_clock = 0U;
    pagecache_last_tick = 0U;
    pagecache_writeback_fail_streak = 0U;
    pagecache_writeback_blocked_until = 0U;
}

bool pagecache_get_or_create(inode_t *inode, uint64_t file_off, uint64_t *pa_out) {
    if (!inode || !pa_out || !pagecache_inode_supported(inode)) {
        return false;
    }
    if ((file_off & (PAGE_SIZE - 1)) != 0) {
        return false;
    }

    pagecache_entry_t *hit = pagecache_find(inode, file_off);
    if (hit) {
        hit->last_touch = pagecache_touch_tick();
        pmm_ref_page((void *)hit->pa);
        *pa_out = hit->pa;
        return true;
    }

    void *page = pmm_alloc_page();
    if (!page && !pagecache_has_free_slot()) {
        pagecache_entry_t *reclaimed = pagecache_slot();
        if (reclaimed) {
            page = pmm_alloc_page();
        }
    }
    if (!page) {
        return false;
    }

    if (!pagecache_load_page(inode, file_off, page)) {
        pmm_free_page(page);
        return false;
    }

    pagecache_entry_t *slot = pagecache_slot();
    if (!slot) {
        pmm_free_page(page);
        return false;
    }

    slot->used = true;
    slot->inode = inode;
    slot->file_off = file_off;
    slot->pa = (uint64_t)page;
    slot->dirty = false;
    slot->last_touch = pagecache_touch_tick();

    /* Cache owns one ref from alloc, mapping caller gets an additional ref. */
    pmm_ref_page(page);
    *pa_out = (uint64_t)page;
    return true;
}

bool pagecache_writeback(inode_t *inode, uint64_t file_off, uint64_t pa, size_t max_bytes) {
    if (!inode || !pagecache_inode_supported(inode)) {
        return false;
    }
    if ((file_off & (PAGE_SIZE - 1)) != 0) {
        return false;
    }
    if (max_bytes == 0) {
        return true;
    }

    pagecache_entry_t *hit = pagecache_find(inode, file_off);
    if (!hit) {
        pagecache_entry_t *slot = pagecache_slot();
        if (!slot) {
            return false;
        }
        slot->used = true;
        slot->inode = inode;
        slot->file_off = file_off;
        slot->pa = pa;
        slot->dirty = false;
        slot->last_touch = pagecache_touch_tick();
        pmm_ref_page((void *)(uintptr_t)pa);
        hit = slot;
    } else if (hit->pa != pa) {
        memcpy((void *)(uintptr_t)hit->pa, (const void *)(uintptr_t)pa, PAGE_SIZE);
    }
    (void)max_bytes;
    uint64_t end = file_off + (uint64_t)max_bytes;
    if (end > (uint64_t)inode->size) {
        inode->size = (size_t)end;
    }
    hit->dirty = true;
    hit->last_touch = pagecache_touch_tick();
    return true;
}

void pagecache_overlay_read(inode_t *inode, size_t file_off, void *buf, size_t len) {
    if (!inode || !buf || len == 0U || !pagecache_inode_supported(inode)) {
        return;
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        size_t cur_off = file_off + done;
        uint64_t page_off = (uint64_t)cur_off & ~(uint64_t)(PAGE_SIZE - 1UL);
        size_t in_page = cur_off & (PAGE_SIZE - 1UL);
        size_t chunk = PAGE_SIZE - in_page;
        pagecache_entry_t *hit;

        if (chunk > len - done) {
            chunk = len - done;
        }
        hit = pagecache_find(inode, page_off);
        if (hit) {
            hit->last_touch = pagecache_touch_tick();
            memcpy(dst + done, (const uint8_t *)(uintptr_t)hit->pa + in_page, chunk);
        }
        done += chunk;
    }
}

void pagecache_notify_write(inode_t *inode, size_t file_off, const void *buf, size_t len) {
    if (!inode || !buf || len == 0U || !pagecache_inode_supported(inode)) {
        return;
    }

    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        size_t cur_off = file_off + done;
        uint64_t page_off = (uint64_t)cur_off & ~(uint64_t)(PAGE_SIZE - 1UL);
        size_t in_page = cur_off & (PAGE_SIZE - 1UL);
        size_t chunk = PAGE_SIZE - in_page;
        pagecache_entry_t *hit;

        if (chunk > len - done) {
            chunk = len - done;
        }
        hit = pagecache_find(inode, page_off);
        if (hit) {
            memcpy((uint8_t *)(uintptr_t)hit->pa + in_page, src + done, chunk);
            hit->dirty = true;
            hit->last_touch = pagecache_touch_tick();
        }
        done += chunk;
    }

    uint64_t end = (uint64_t)file_off + (uint64_t)len;
    if (end > (uint64_t)inode->size) {
        inode->size = (size_t)end;
    }
    pagecache_maybe_writeback_pressure();
}

int pagecache_read(inode_t *inode, size_t *offset, void *buf, size_t len) {
    if (!inode || !offset || !buf || !pagecache_inode_supported(inode)) {
        return -1;
    }
    if (len == 0U) {
        return 0;
    }
    if (*offset >= inode->size) {
        return 0;
    }

    size_t n = len;
    if (*offset + n > inode->size) {
        n = inode->size - *offset;
    }

    size_t done = 0U;
    uint8_t *dst = (uint8_t *)buf;
    while (done < n) {
        size_t cur_off = *offset + done;
        uint64_t page_off = (uint64_t)cur_off & ~(uint64_t)(PAGE_SIZE - 1UL);
        size_t in_page = cur_off & (PAGE_SIZE - 1UL);
        size_t chunk = PAGE_SIZE - in_page;
        uint64_t pa = 0;
        if (chunk > n - done) {
            chunk = n - done;
        }
        if (!pagecache_get_or_create(inode, page_off, &pa)) {
            break;
        }
        memcpy(dst + done, (const uint8_t *)(uintptr_t)pa + in_page, chunk);
        pmm_free_page((void *)(uintptr_t)pa);
        done += chunk;
    }
    if (done == 0U) {
        return -1;
    }
    *offset += done;
    return (int)done;
}

int pagecache_write(inode_t *inode, size_t *offset, const void *buf, size_t len) {
    if (!inode || !offset || !buf || !pagecache_inode_supported(inode) || !inode->writable) {
        return -1;
    }
    if (len == 0U) {
        return 0;
    }

    size_t n = len;
    if (inode->type == INODE_DEV) {
        if (*offset >= inode->size) {
            return -1;
        }
        if (*offset + n > inode->size) {
            n = inode->size - *offset;
        }
    }
    if (inode->fs_kind == FS_KIND_MEM) {
        if (!inode->data || *offset >= inode->capacity) {
            return -1;
        }
        size_t cap_rem = inode->capacity - *offset;
        if (n > cap_rem) {
            n = cap_rem;
        }
    }

    size_t done = 0U;
    const uint8_t *src = (const uint8_t *)buf;
    while (done < n) {
        size_t cur_off = *offset + done;
        uint64_t page_off = (uint64_t)cur_off & ~(uint64_t)(PAGE_SIZE - 1UL);
        size_t in_page = cur_off & (PAGE_SIZE - 1UL);
        size_t chunk = PAGE_SIZE - in_page;
        uint64_t pa = 0;
        if (chunk > n - done) {
            chunk = n - done;
        }
        if (!pagecache_get_or_create(inode, page_off, &pa)) {
            break;
        }
        memcpy((uint8_t *)(uintptr_t)pa + in_page, src + done, chunk);
        pagecache_entry_t *hit = pagecache_find(inode, page_off);
        if (hit) {
            hit->dirty = true;
            hit->last_touch = pagecache_touch_tick();
        }
        pmm_free_page((void *)(uintptr_t)pa);
        done += chunk;
    }

    if (done == 0U) {
        return -1;
    }
    uint64_t end = (uint64_t)(*offset) + done;
    if (end > inode->size) {
        inode->size = (size_t)end;
    }
    *offset += done;
    pagecache_maybe_writeback_pressure();
    return (int)done;
}

void pagecache_mark_dirty(inode_t *inode, uint64_t file_off, size_t len) {
    if (!inode || len == 0U || !pagecache_inode_supported(inode)) {
        return;
    }
    size_t done = 0U;
    while (done < len) {
        uint64_t cur = file_off + done;
        uint64_t page_off = cur & ~(uint64_t)(PAGE_SIZE - 1UL);
        size_t in_page = (size_t)(cur & (PAGE_SIZE - 1UL));
        size_t chunk = PAGE_SIZE - in_page;
        pagecache_entry_t *hit = pagecache_find(inode, page_off);
        if (chunk > len - done) {
            chunk = len - done;
        }
        if (hit) {
            hit->dirty = true;
            hit->last_touch = pagecache_touch_tick();
        }
        done += chunk;
    }
    pagecache_maybe_writeback_pressure();
}

int pagecache_flush_inode(inode_t *inode) {
    if (!inode) {
        return 0;
    }
    return pagecache_flush_match(inode, 0U, 0U, false, (uint32_t)MAX_FILE_CACHE_PAGES);
}

int pagecache_flush_inode_range(inode_t *inode, uint64_t start_off, uint64_t len) {
    if (!inode) {
        return -1;
    }
    return pagecache_flush_match(inode, start_off, len, true,
                                 (uint32_t)MAX_FILE_CACHE_PAGES);
}

int pagecache_invalidate_inode_range(inode_t *inode, uint64_t start_off, uint64_t len) {
    if (!inode) {
        return -1;
    }
    bool busy = false;

    for (int i = 0; i < MAX_FILE_CACHE_PAGES; i++) {
        pagecache_entry_t *e = &pagecache[i];
        if (!e->used || e->inode != inode) {
            continue;
        }
        if (!pagecache_range_overlaps(e, start_off, len)) {
            continue;
        }

        /*
         * Keep entries pinned by active mappings/tasks; dropping only the
         * cache-only ref preserves correctness for MAP_SHARED writers.
         */
        if (pmm_page_refcount((const void *)(uintptr_t)e->pa) > 1U) {
            busy = true;
            continue;
        }

        pmm_free_page((void *)(uintptr_t)e->pa);
        memset(e, 0, sizeof(*e));
    }
    return busy ? -1 : 0;
}

int pagecache_flush_all(void) {
    return pagecache_flush_match(0, 0U, 0U, false, (uint32_t)MAX_FILE_CACHE_PAGES);
}

void pagecache_tick(uint64_t now_ticks) {
    static uint64_t last_writeback_tick;
    pagecache_last_tick = now_ticks;
    if (now_ticks - last_writeback_tick < PAGECACHE_WRITEBACK_PERIOD) {
        return;
    }
    last_writeback_tick = now_ticks;
    if (pagecache_writeback_blocked()) {
        return;
    }
    uint32_t dirty = pagecache_dirty_count();
    if (dirty == 0U) {
        return;
    }
    uint32_t budget = PAGECACHE_WRITEBACK_BUDGET + (dirty / 8U);
    if (budget > PAGECACHE_WRITEBACK_MAX_BUDGET) {
        budget = PAGECACHE_WRITEBACK_MAX_BUDGET;
    }
    (void)pagecache_flush_match(0, 0U, 0U, false, budget);
}

void pagecache_invalidate_inode(inode_t *inode) {
    if (!inode) {
        return;
    }
    for (int i = 0; i < MAX_FILE_CACHE_PAGES; i++) {
        if (!pagecache[i].used || pagecache[i].inode != inode) {
            continue;
        }
        /*
         * Keep pages that are still referenced by active mappings/tasks.
         * They will naturally age out once refcount drops back to cache-only.
         */
        if (pmm_page_refcount((const void *)(uintptr_t)pagecache[i].pa) > 1U) {
            if (inode->size <= pagecache[i].file_off) {
                pagecache[i].dirty = false;
            }
            continue;
        }
        pmm_free_page((void *)(uintptr_t)pagecache[i].pa);
        memset(&pagecache[i], 0, sizeof(pagecache[i]));
    }
}
