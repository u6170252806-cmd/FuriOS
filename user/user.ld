ENTRY(_start)

SECTIONS
{
    . = 0x00400000;

    .text : ALIGN(4096)
    {
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
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : { *(.note*) *(.comment*) }
}
