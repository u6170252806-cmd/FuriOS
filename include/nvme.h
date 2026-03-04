#ifndef FUROS_NVME_H
#define FUROS_NVME_H

#include <stdint.h>
#include <stdbool.h>

#define NVME_MAX_CONTROLLERS 8U
#define NVME_MAX_NAMESPACES  8U
#define NVME_SECTOR_SIZE_512 512U

void nvme_init(void);
void nvme_poll(void);
bool nvme_ready(void);
uint32_t nvme_controller_count(void);
bool nvme_controller_ready(uint32_t ctrl);
bool nvme_ns_present(uint32_t ctrl, uint32_t nsid);
uint64_t nvme_ns_capacity_sectors512(uint32_t ctrl, uint32_t nsid);
uint32_t nvme_ns_sector_size(uint32_t ctrl, uint32_t nsid);
int nvme_ns_rw(uint32_t ctrl, uint32_t nsid, uint64_t lba512, void *buf, uint32_t count512, bool write);
int nvme_ns_rw_sector(uint32_t ctrl, uint32_t nsid, uint64_t lba512, void *buf, bool write);
int nvme_ns_flush(uint32_t ctrl, uint32_t nsid);
bool nvme_handle_irq(uint32_t intid);

#endif
