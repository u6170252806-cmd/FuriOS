.section .text
.global _start
.type _start, %function
_start:
    mov x29, xzr
    mov x30, xzr
    bl main
    mov x0, x0
    mov x8, #4
    svc #0
1:
    wfe
    b 1b
