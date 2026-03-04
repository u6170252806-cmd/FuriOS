#ifndef FUROS_PAGECACHE_H
#define FUROS_PAGECACHE_H

#include <stdint.h>
#include <stdbool.h>
#include "fs.h"

void pagecache_init(void);
bool pagecache_get_or_create(inode_t *inode, uint64_t file_off, uint64_t *pa_out);
bool pagecache_writeback(inode_t *inode, uint64_t file_off, uint64_t pa, size_t max_bytes);
int pagecache_read(inode_t *inode, size_t *offset, void *buf, size_t len);
int pagecache_write(inode_t *inode, size_t *offset, const void *buf, size_t len);
void pagecache_mark_dirty(inode_t *inode, uint64_t file_off, size_t len);
int pagecache_flush_inode(inode_t *inode);
int pagecache_flush_inode_range(inode_t *inode, uint64_t start_off, uint64_t len);
int pagecache_invalidate_inode_range(inode_t *inode, uint64_t start_off, uint64_t len);
int pagecache_flush_all(void);
void pagecache_tick(uint64_t now_ticks);
void pagecache_overlay_read(inode_t *inode, size_t file_off, void *buf, size_t len);
void pagecache_notify_write(inode_t *inode, size_t file_off, const void *buf, size_t len);
void pagecache_invalidate_inode(inode_t *inode);

#endif
