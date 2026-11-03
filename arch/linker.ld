ENTRY(start)

PHDRS {
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)); /* Read + Execute */
    rodata  PT_LOAD    FLAGS((1 << 0));            /* Read-Only */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 0)); /* Read + Write */
}

SECTIONS {
    /* Standard higher-half or 1MB lower-half load address */
    . = 1M;

    .text : {
        KEEP(*(.multiboot_header))
        *(.text .text.*)
    } :text

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    } :rodata

    .data : ALIGN(4K) {
        *(.data .data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    . = ALIGN(4K);
    PROVIDE(_kernel_end = .);

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}