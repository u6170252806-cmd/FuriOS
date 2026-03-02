#ifndef FUROS_TIMER_H
#define FUROS_TIMER_H

#include <stdint.h>

void timer_init(uint32_t hz);
void timer_handle_irq(void);
uint64_t timer_ticks(void);

#endif
