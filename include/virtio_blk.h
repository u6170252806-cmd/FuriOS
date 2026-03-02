#ifndef FUROS_VIRTIO_BLK_H
#define FUROS_VIRTIO_BLK_H

#include <stdint.h>
#include <stdbool.h>

#define VIRTIO_BLK_SECTOR_SIZE 512U

void virtio_blk_init(void);
bool virtio_blk_ready(void);
uint64_t virtio_blk_capacity_sectors(void);
uint32_t virtio_blk_block_size(void);
int virtio_blk_rw_sector(uint64_t lba, void *buf, bool write);
int virtio_blk_flush(void);
bool virtio_blk_handle_irq(uint32_t intid);

#endif
