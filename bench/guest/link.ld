/* S5LBox — guest benchmark image layout. VA 0 is mapped onto the start of
 * DRAM by tools/guest_bench.c; everything here is a virtual address. */
ENTRY(_start)

SECTIONS
{
    . = 0x0;
    .vectors : { KEEP(*(.vectors)) }

    . = 0x1000;
    .mbox : { KEEP(*(.mbox)) }

    . = 0x2000;
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    . = ALIGN(4096);
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) }
    . = ALIGN(4096);
    __bss_end = .;

    __usr_stack_top = 0x00e00000;
    __svc_stack_top = 0x00f00000;

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
