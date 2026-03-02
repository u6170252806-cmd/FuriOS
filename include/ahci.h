#ifndef FUROS_AHCI_H
#define FUROS_AHCI_H

#include <stdint.h>
#include <stdbool.h>

void ahci_init(void);
bool ahci_ready(void);
uint32_t ahci_disk_count(void);
bool ahci_disk_present(uint32_t index);
uint64_t ahci_disk_capacity_sectors(uint32_t index);
int ahci_rw(uint32_t index, uint64_t lba, void *buf, uint32_t count, bool write);
int ahci_rw_sector(uint32_t index, uint64_t lba, void *buf, bool write);
int ahci_flush(uint32_t index);
void ahci_poll(void);
bool ahci_handle_irq(uint32_t intid);

#endif
