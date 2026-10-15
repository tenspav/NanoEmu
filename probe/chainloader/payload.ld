/*
 * payload.ld -- Linker script for bare-metal payload at physical 0x08000000
 *
 * Cortex-A5, flat physical memory, DRAM base 0x08000000.
 * Freestanding, no libc, no dynamic allocation.
 */

OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    DRAM (rwx) : ORIGIN = 0x08000000, LENGTH = 64M
}

SECTIONS
{
    . = 0x08000000;

    .text : {
        __text_start = .;
        *(.text.entry)
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        __text_end = .;
    } > DRAM

    .data : {
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > DRAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > DRAM

    .stack (NOLOAD) : {
        . = ALIGN(8);
        . += 0x4000; /* 16 KiB DRAM stack */
        _stack_top = .;
    } > DRAM

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.note*)
        *(.comment)
    }
}
