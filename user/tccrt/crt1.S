.section .text
.global _start
.type _start, %function
_start:
    mov x29, xzr
    mov x30, xzr
    mov x19, x0
    mov x20, x1
    mov x22, x2
    mov x23, x3

    adrp x9, __init_array_start
    add x9, x9, :lo12:__init_array_start
    adrp x10, __init_array_end
    add x10, x10, :lo12:__init_array_end
1:
    cmp x9, x10
    b.hs 2f
    ldr x11, [x9], #8
    cbz x11, 1b
    blr x11
    b 1b
2:
    mov x0, x19
    mov x1, x20
    mov x2, x22
    mov x3, x23
    bl main
    mov x21, x0

    adrp x9, __fini_array_start
    add x9, x9, :lo12:__fini_array_start
    adrp x10, __fini_array_end
    add x10, x10, :lo12:__fini_array_end
    cmp x10, x9
    b.ls 4f
3:
    sub x10, x10, #8
    ldr x11, [x10]
    cbz x11, 5f
    blr x11
5:
    cmp x10, x9
    b.hi 3b
4:
    mov x0, x21
    mov x8, #4
    svc #0
6:
    wfe
    b 6b
