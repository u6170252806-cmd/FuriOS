#include "pagecache.h"
#include "config.h"
#include "ext4.h"
#include "pmm.h"
#include "string.h"

typedef struct {
    inode_t *inode;
    uint64_t file_off;
    uint64_t pa;
    bool used;
} pagecache_entry_t;

static pagecache_entry_t pagecache[MAX_FILE_CACHE_PAGES];

static bool pagecache_inode_supported(const inode_t *inode) {
    if (!inode || inode->type != INODE_FILE) {
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
    return 0;
}

void pagecache_init(void) {
    memset(pagecache, 0, sizeof(pagecache));
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
        pmm_ref_page((void *)hit->pa);
        *pa_out = hit->pa;
        return true;
    }

    if (pmm_available_pages() <= PMM_OOM_RESERVE_PAGES) {
        return false;
    }
    void *page = pmm_alloc_page();
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

    if (max_bytes == 0U) {
        return true;
    }
    if (!pagecache_store_page(inode, file_off, (const void *)pa, max_bytes)) {
        return false;
    }

    pagecache_entry_t *hit = pagecache_find(inode, file_off);
    if (hit) {
        hit->pa = pa;
    }
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
        }
        done += chunk;
    }
}

void pagecache_invalidate_inode(inode_t *inode) {
    if (!inode) {
        return;
    }
    for (int i = 0; i < MAX_FILE_CACHE_PAGES; i++) {
        if (!pagecache[i].used || pagecache[i].inode != inode) {
            continue;
        }
        pmm_free_page((void *)(uintptr_t)pagecache[i].pa);
        memset(&pagecache[i], 0, sizeof(pagecache[i]));
    }
}
