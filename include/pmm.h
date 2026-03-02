#ifndef FUROS_PMM_H
#define FUROS_PMM_H

#include <stddef.h>
#include <stdint.h>

void pmm_init(uint64_t start, uint64_t end);
void *pmm_alloc(size_t bytes, size_t align);
void *pmm_alloc_page(void);
void pmm_ref_page(void *page);
uint32_t pmm_page_refcount(const void *page);
void pmm_free_page(void *page);
size_t pmm_total_pages(void);
size_t pmm_used_pages(void);
size_t pmm_available_pages(void);

#endif
