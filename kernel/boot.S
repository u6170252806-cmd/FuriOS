.section .text.boot
.align 7
.global _start
.type _start, %function

_start:
    mrs x0, CurrentEL
    lsr x0, x0, #2
    adrp x1, boot_current_el
    add x1, x1, :lo12:boot_current_el
    str x0, [x1]

    cmp x0, #3
    b.eq el3_to_el2
    cmp x0, #2
    b.eq el2_to_el1
    cmp x0, #1
    b.eq el1_entry

0:
    wfe
    b 0b

el3_to_el2:
    ldr x1, =boot_stack_top
    msr sp_el2, x1

    mov x1, #0x501          /* SCR_EL3: NS=1, HCE=1, RW=1 */ 
    msr scr_el3, x1

    mov x1, #0x3C9          /* D,A,I,F masked + EL2h */
    msr spsr_el3, x1

    adr x1, el2_to_el1
    msr elr_el3, x1
    eret

el2_to_el1:  /* EL2 -> EL1 */
    ldr x1, =boot_stack_top
    msr sp_el1, x1

    mov x1, #(1 << 31)      /* HCR_EL2.RW=1 => EL1 AArch64 */
    msr hcr_el2, x1

    mov x1, #0
    msr cptr_el2, x1

    mov x1, #3              /* allow EL1 physical timer access */
    msr cnthctl_el2, x1
    mov x1, #0
    msr cntvoff_el2, x1

    mov x1, #0x3C5          /* D,A,I,F masked + EL1h */
    msr spsr_el2, x1

    adr x1, el1_entry
    msr elr_el2, x1
    eret

el1_entry:
    ldr x0, =boot_stack_top
    mov sp, x0
    adr x0, vector_table
    msr vbar_el1, x0
    isb

    bl zero_bss
    bl kernel_main

2:
    wfe
    b 2b

.type zero_bss, %function
zero_bss:
    ldr x0, =__bss_start
    ldr x1, =__bss_end
    mov x2, #0
1:
    cmp x0, x1
    b.hs 2f
    str x2, [x0], #8
    b 1b
2:
    ret

.section .data
.align 3
.global boot_current_el
boot_current_el:
    .quad 0

.section .bss.stack
.align 12
boot_stack:
    .skip 16384
.global boot_stack_top
boot_stack_top:
