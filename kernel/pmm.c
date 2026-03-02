#include "pmm.h"
#include "config.h"
#include "string.h"

static uint64_t pmm_cur;
static uint64_t pmm_end;
static uint64_t free_pages[PHYS_RAM_SIZE / PAGE_SIZE];
static uint32_t page_refs[PHYS_RAM_SIZE / PAGE_SIZE];
static size_t free_top;
static size_t owned_pages;

static uint64_t align_up(uint64_t x, uint64_t align) {
    return (x + align - 1u) & ~(align - 1u);
}

static uint64_t align_down(uint64_t x, uint64_t align) {
    return x & ~(align - 1u);
}

static int page_index(uint64_t p, size_t *idx_out) {
    if ((p & (PAGE_SIZE - 1UL)) != 0) {
        return -1;
    }
    if (p < PHYS_RAM_BASE || p >= PHYS_RAM_BASE + PHYS_RAM_SIZE) {
        return -1;
    }
    *idx_out = (size_t)((p - PHYS_RAM_BASE) / PAGE_SIZE);
    return 0;
}

static void mark_page_owned(uint64_t p) {
    size_t idx = 0;
    if (page_index(p, &idx) != 0) {
        return;
    }
    if (page_refs[idx] == 0) {
        page_refs[idx] = 1;
        owned_pages++;
    }
}

static void mark_range_owned(uint64_t base, size_t bytes) {
    if (bytes == 0) {
        return;
    }
    uint64_t end = base + (uint64_t)bytes;
    if (end < base) {
        return;
    }
    uint64_t p = align_up(base, PAGE_SIZE);
    uint64_t limit = align_down(end, PAGE_SIZE);
    while (p < limit) {
        mark_page_owned(p);
        p += PAGE_SIZE;
    }
}

void pmm_init(uint64_t start, uint64_t end) {
    pmm_cur = align_up(start, 16);
    pmm_end = end;
    free_top = 0;
    owned_pages = 0;
    memset(page_refs, 0, sizeof(page_refs));
}

void *pmm_alloc(size_t bytes, size_t align) {
    uint64_t cur = align_up(pmm_cur, (uint64_t)align);
    uint64_t next = cur + bytes;
    if (next > pmm_end || next < cur) {
        return 0;
    }
    pmm_cur = next;
    mark_range_owned(cur, bytes);
    memset((void *)cur, 0, bytes);
    return (void *)cur;
}

void *pmm_alloc_page(void) {
    if (free_top > 0) {
        uint64_t p = free_pages[--free_top];
        size_t idx = 0;
        if (page_index(p, &idx) == 0) {
            page_refs[idx] = 1;
            owned_pages++;
        }
        void *page = (void *)p;
        memset(page, 0, PAGE_SIZE);
        return page;
    }
    return pmm_alloc(4096, 4096);
}

void pmm_ref_page(void *page) {
    uint64_t p = (uint64_t)page;
    size_t idx = 0;
    if (page_index(p, &idx) != 0) {
        return;
    }
    if (page_refs[idx] == 0) {
        page_refs[idx] = 1;
        owned_pages++;
        return;
    }
    page_refs[idx]++;
}

uint32_t pmm_page_refcount(const void *page) {
    uint64_t p = (uint64_t)page;
    size_t idx = 0;
    if (page_index(p, &idx) != 0) {
        return 0;
    }
    return page_refs[idx];
}

void pmm_free_page(void *page) {
    uint64_t p = (uint64_t)page;
    size_t idx = 0;
    if (!page) {
        return;
    }
    if (page_index(p, &idx) != 0) {
        return;
    }

    if (page_refs[idx] == 0) {
        return;
    }
    page_refs[idx]--;
    if (page_refs[idx] != 0) {
        return;
    }
    if (owned_pages > 0) {
        owned_pages--;
    }

    if (free_top < (sizeof(free_pages) / sizeof(free_pages[0]))) {
        free_pages[free_top++] = p;
    }
}

size_t pmm_total_pages(void) {
    return PHYS_RAM_SIZE / PAGE_SIZE;
}

size_t pmm_used_pages(void) {
    return owned_pages;
}

size_t pmm_available_pages(void) {
    uint64_t cur = align_up(pmm_cur, PAGE_SIZE);
    size_t bump_pages = 0;
    if (cur < pmm_end) {
        bump_pages = (size_t)((pmm_end - cur) / PAGE_SIZE);
    }
    return free_top + bump_pages;
}
