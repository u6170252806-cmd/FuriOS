#ifndef FUROS_KERNEL_H
#define FUROS_KERNEL_H

#include <stdint.h>
#include "task.h"

extern uint64_t boot_current_el;
extern uint8_t __bss_start[];
extern uint8_t __bss_end[];
extern uint8_t __heap_start[];
extern uint8_t __heap_end[];
extern uint8_t vector_table[];

void kernel_main(void);
void panic(const char *msg);
void trap_init(void);
trapframe_t *trap_handle_sync(trapframe_t *tf, uint64_t esr, uint64_t far);
trapframe_t *trap_handle_irq(trapframe_t *tf);
trapframe_t *trap_handle_fiq(trapframe_t *tf);
trapframe_t *trap_handle_serror(trapframe_t *tf);

#endif
