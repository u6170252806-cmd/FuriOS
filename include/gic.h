#ifndef FUROS_GIC_H
#define FUROS_GIC_H

#include <stdint.h>

void gic_init(void);
void gic_enable_irq(uint32_t intid, uint8_t priority);
uint32_t gic_ack_irq(void);
void gic_eoi_irq(uint32_t iar);

#endif
