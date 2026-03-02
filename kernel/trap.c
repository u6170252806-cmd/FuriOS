#include "kernel.h"
#include "task.h"
#include "uart.h"
#include "print.h"
#include "config.h"
#include "gic.h"
#include "timer.h"
#include "virtio_blk.h"
#include "ahci.h"
#include "nvme.h"

long syscall_dispatch(trapframe_t *tf, bool *force_resched);

#define ESR_EC_SHIFT 26
#define ESR_EC_MASK  0x3FUL
#define ESR_EC_SVC64 0x15
#define ESR_EC_IABT_LOW 0x20
#define ESR_EC_IABT_CUR 0x21
#define ESR_EC_DABT_LOW 0x24
#define ESR_EC_DABT_CUR 0x25
#define ESR_ISS_WNR (1UL << 6)
#define ESR_ISS_FSC_MASK 0x3FUL
#define ESR_FSC_TYPE_MASK 0x3CUL
#define ESR_FSC_FAULT 0x04UL

static void print_trap(const char *name, uint64_t esr, uint64_t far) {
    uart_puts("[trap] ");
    uart_puts(name);
    uart_puts(" esr=");
    print_hex64(esr);
    uart_puts(" far=");
    print_hex64(far);
    uart_puts("\n");
}

trapframe_t *trap_handle_sync(trapframe_t *tf, uint64_t esr, uint64_t far) {
    uint64_t ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

    if (ec == ESR_EC_SVC64) {
        bool force_resched = false;
        syscall_dispatch(tf, &force_resched);
        return task_schedule_from_trap(tf, force_resched);
    }

    bool is_instr_abort = (ec == ESR_EC_IABT_LOW || ec == ESR_EC_IABT_CUR);
    bool is_data_abort = (ec == ESR_EC_DABT_LOW || ec == ESR_EC_DABT_CUR);
    bool is_current_abort = (ec == ESR_EC_IABT_CUR || ec == ESR_EC_DABT_CUR);
    if ((is_instr_abort || is_data_abort) && task_current()) {
        bool is_write = is_data_abort && ((esr & ESR_ISS_WNR) != 0);
        uint64_t fsc = esr & ESR_ISS_FSC_MASK;
        bool is_translation = (fsc & ESR_FSC_TYPE_MASK) == ESR_FSC_FAULT;
        if (task_handle_page_fault(far, is_write, is_translation, is_instr_abort, tf->sp_el0)) {
            return task_schedule_from_trap(tf, false);
        }
    }

    print_trap("sync", esr, far);
    if (task_current()) {
        if (is_current_abort && (far < USER_VA_BASE || far >= USER_VA_LIMIT)) {
            panic("kernel page fault");
        }
        task_mark_exit(-1);
        return task_schedule_from_trap(tf, true);
    }
    panic("sync exception in kernel");
    return tf;
}

trapframe_t *trap_handle_irq(trapframe_t *tf) {
    uint32_t iar = gic_ack_irq();
    uint32_t intid = iar & 0x3FFU;
    bool force_resched = false;
    bool from_el0 = (tf->spsr_el1 & 0xFUL) == 0;

    if (intid == TIMER_IRQ) {
        timer_handle_irq();
        if (task_wake_sleepers(timer_ticks())) {
            force_resched = from_el0;
        }
        if (from_el0) {
            force_resched = true;
        }
    } else if (virtio_blk_handle_irq(intid)) {
        /* handled */
    } else if (ahci_handle_irq(intid)) {
        /* handled */
    } else if (nvme_handle_irq(intid)) {
        /* handled */
    } else if (intid != 1023U) {
        uart_puts("[trap] unexpected irq=");
        print_dec((int)intid);
        uart_puts("\n");
    }

    gic_eoi_irq(iar);

    if (force_resched) {
        return task_schedule_from_trap(tf, true);
    }
    return tf;
}

trapframe_t *trap_handle_fiq(trapframe_t *tf) {
    uart_puts("[trap] fiq\n");
    return task_schedule_from_trap(tf, true);
}

trapframe_t *trap_handle_serror(trapframe_t *tf) {
    uart_puts("[trap] serror\n");
    if (task_current()) {
        task_mark_exit(-1);
        return task_schedule_from_trap(tf, true);
    }
    panic("SError in kernel");
    return tf;
}
