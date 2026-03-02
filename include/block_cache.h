#ifndef FUROS_BLOCK_CACHE_H
#define FUROS_BLOCK_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs.h"

void block_cache_init(void);
int block_cache_attach_inode(const inode_t *inode);
dev_kind_t block_cache_device(void);
bool block_cache_ready(void);
uint64_t block_cache_capacity_sectors(void);
int block_cache_read(uint64_t lba, void *buf, size_t count);
int block_cache_write(uint64_t lba, const void *buf, size_t count);
int block_cache_flush(void);

#endif
