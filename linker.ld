ENTRY(_start)

SECTIONS
{
    . = 0x40200000;

    .text : ALIGN(4096)
    {
        KEEP(*(.text.boot))
        *(.text.vectors)
        *(.text*)
    }

    .rodata : ALIGN(4096)
    {
        *(.rodata*)
    }

    .data : ALIGN(4096)
    {
        *(.data*)
    }

    .bss (NOLOAD) : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(4096);
    __heap_start = .;
    . = 0x4F000000;
    __heap_end = .;
}
