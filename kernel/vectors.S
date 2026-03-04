.set TF_X0,      0
.set TF_X2,      16
.set TF_X4,      32
.set TF_X6,      48
.set TF_X8,      64
.set TF_X10,     80
.set TF_X12,     96
.set TF_X14,     112
.set TF_X16,     128
.set TF_X18,     144
.set TF_X20,     160
.set TF_X22,     176
.set TF_X24,     192
.set TF_X26,     208
.set TF_X28,     224
.set TF_X30,     240
.set TF_SP_EL0,  248
.set TF_ELR_EL1, 256
.set TF_SPSR_EL1,264
.set TF_TPIDR_EL0,272
.set TF_RESERVED0,280
.set TF_SIZE,    288

.macro SAVE_TF
    sub sp, sp, #TF_SIZE
    stp x0,  x1,  [sp, #TF_X0]
    stp x2,  x3,  [sp, #TF_X2]
    stp x4,  x5,  [sp, #TF_X4]
    stp x6,  x7,  [sp, #TF_X6]
    stp x8,  x9,  [sp, #TF_X8]
    stp x10, x11, [sp, #TF_X10]
    stp x12, x13, [sp, #TF_X12]
    stp x14, x15, [sp, #TF_X14]
    stp x16, x17, [sp, #TF_X16]
    stp x18, x19, [sp, #TF_X18]
    stp x20, x21, [sp, #TF_X20]
    stp x22, x23, [sp, #TF_X22]
    stp x24, x25, [sp, #TF_X24]
    stp x26, x27, [sp, #TF_X26]
    stp x28, x29, [sp, #TF_X28]
    str x30, [sp, #TF_X30]

    mrs x1, sp_el0
    str x1, [sp, #TF_SP_EL0]
    mrs x1, elr_el1
    str x1, [sp, #TF_ELR_EL1]
    mrs x1, spsr_el1
    str x1, [sp, #TF_SPSR_EL1]
    mrs x1, tpidr_el0
    str x1, [sp, #TF_TPIDR_EL0]
.endm

.macro RESTORE_FROM_X0
    mov x9, x0
    add sp, x9, #TF_SIZE

    ldr x1, [x9, #TF_SP_EL0]
    msr sp_el0, x1
    ldr x1, [x9, #TF_ELR_EL1]
    msr elr_el1, x1
    ldr x1, [x9, #TF_SPSR_EL1]
    msr spsr_el1, x1
    ldr x1, [x9, #TF_TPIDR_EL0]
    msr tpidr_el0, x1

    ldp x0,  x1,  [x9, #TF_X0]
    ldp x2,  x3,  [x9, #TF_X2]
    ldp x4,  x5,  [x9, #TF_X4]
    ldp x6,  x7,  [x9, #TF_X6]
    ldr x8,  [x9, #TF_X8]
    ldp x10, x11, [x9, #TF_X10]
    ldp x12, x13, [x9, #TF_X12]
    ldp x14, x15, [x9, #TF_X14]
    ldp x16, x17, [x9, #TF_X16]
    ldp x18, x19, [x9, #TF_X18]
    ldr x21, [x9, #TF_X20 + 8]
    ldp x22, x23, [x9, #TF_X22]
    ldp x24, x25, [x9, #TF_X24]
    ldp x26, x27, [x9, #TF_X26]
    ldp x28, x29, [x9, #TF_X28]
    ldr x30, [x9, #TF_X30]
    ldr x20, [x9, #TF_X20]
    ldr x9,  [x9, #TF_X8 + 8]
    eret
.endm

.macro VECTOR target
    b \target
    .space 124
.endm

.section .text.vectors
.align 11
.global vector_table
vector_table:
    VECTOR sync_current_sp0
    VECTOR irq_current_sp0
    VECTOR fiq_current_sp0
    VECTOR serr_current_sp0

    VECTOR sync_current_spx
    VECTOR irq_current_spx
    VECTOR fiq_current_spx
    VECTOR serr_current_spx

    VECTOR sync_lower_a64
    VECTOR irq_lower_a64
    VECTOR fiq_lower_a64
    VECTOR serr_lower_a64

    VECTOR sync_lower_a32
    VECTOR irq_lower_a32
    VECTOR fiq_lower_a32
    VECTOR serr_lower_a32

sync_current_sp0:
sync_current_spx:
sync_lower_a64:
    SAVE_TF
    mrs x1, esr_el1
    mrs x2, far_el1
    mov x0, sp
    bl trap_handle_sync
    RESTORE_FROM_X0

irq_current_sp0:
irq_current_spx:
irq_lower_a64:
    SAVE_TF
    mov x0, sp
    bl trap_handle_irq
    RESTORE_FROM_X0

fiq_current_sp0:
fiq_current_spx:
fiq_lower_a64:
    SAVE_TF
    mov x0, sp
    bl trap_handle_fiq
    RESTORE_FROM_X0

serr_current_sp0:
serr_current_spx:
serr_lower_a64:
    SAVE_TF
    mov x0, sp
    bl trap_handle_serror
    RESTORE_FROM_X0

sync_lower_a32:
irq_lower_a32:
fiq_lower_a32:
serr_lower_a32:
    b sync_current_spx

.section .text
.align 4
.global trap_enter_first
.type trap_enter_first, %function
trap_enter_first:
    /* x0 = trapframe*, x1 = ttbr0 */
    msr ttbr0_el1, x1
    dsb ish
    tlbi vmalle1
    dsb ish
    isb

    mov x9, x0
    add sp, x9, #TF_SIZE
    ldr x2, [x9, #TF_SP_EL0]
    msr sp_el0, x2
    ldr x2, [x9, #TF_ELR_EL1]
    msr elr_el1, x2
    ldr x2, [x9, #TF_SPSR_EL1]
    msr spsr_el1, x2
    ldr x2, [x9, #TF_TPIDR_EL0]
    msr tpidr_el0, x2

    ldp x0,  x1,  [x9, #TF_X0]
    ldp x2,  x3,  [x9, #TF_X2]
    ldp x4,  x5,  [x9, #TF_X4]
    ldp x6,  x7,  [x9, #TF_X6]
    ldr x8,  [x9, #TF_X8]
    ldp x10, x11, [x9, #TF_X10]
    ldp x12, x13, [x9, #TF_X12]
    ldp x14, x15, [x9, #TF_X14]
    ldp x16, x17, [x9, #TF_X16]
    ldp x18, x19, [x9, #TF_X18]
    ldr x21, [x9, #TF_X20 + 8]
    ldp x22, x23, [x9, #TF_X22]
    ldp x24, x25, [x9, #TF_X24]
    ldp x26, x27, [x9, #TF_X26]
    ldp x28, x29, [x9, #TF_X28]
    ldr x30, [x9, #TF_X30]
    ldr x20, [x9, #TF_X20]
    ldr x9,  [x9, #TF_X8 + 8]
    eret
