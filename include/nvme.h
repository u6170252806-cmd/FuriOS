#ifndef FUROS_NVME_H
#define FUROS_NVME_H

#include <stdint.h>
#include <stdbool.h>

#define NVME_SECTOR_SIZE_512 512U

void nvme_init(void);
void nvme_poll(void);
bool nvme_ready(void);
uint32_t nvme_disk_count(void);
bool nvme_disk_present(uint32_t index);
uint64_t nvme_disk_capacity_sectors(uint32_t index);
uint32_t nvme_disk_sector_size(uint32_t index);
int nvme_rw(uint32_t index, uint64_t lba, void *buf, uint32_t count, bool write);
int nvme_rw_sector(uint32_t index, uint64_t lba, void *buf, bool write);
int nvme_flush(uint32_t index);
bool nvme_handle_irq(uint32_t intid);

#endif
