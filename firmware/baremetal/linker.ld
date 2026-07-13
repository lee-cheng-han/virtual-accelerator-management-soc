OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    SRAM (rwx) : ORIGIN = 0x80000000, LENGTH = 512K
}

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = ORIGIN(SRAM);
    __sram_start = .;

    .text : ALIGN(4)
    {
        KEEP(*(.text.start))
        *(.text .text.*)
    } > SRAM :text

    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.*)
    } > SRAM :text

    .data : ALIGN(4)
    {
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
        *(.data .data.*)
    } > SRAM :data

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } > SRAM :data

    . = ALIGN(16);
    __image_end = .;
    __stack_top = ORIGIN(SRAM) + LENGTH(SRAM);
    __sram_end = __stack_top;

    ASSERT(__image_end < __stack_top - 4096,
           "firmware image leaves less than 4 KiB for stack")

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame.*)
    }
}
