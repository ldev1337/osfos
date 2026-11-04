ENTRY(start)

PHDRS {
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)); /* Read + Execute */
    rodata  PT_LOAD    FLAGS((1 << 0));            /* Read-Only */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 0)); /* Read + Write */
}

SECTIONS {
    /* Align base load address to 2MB huge-page boundary */
    . = 2M;

    .text : {
        KEEP(*(.multiboot_header))
        *(.text .text.*)
    } :text

    .rodata : ALIGN(2M) {
        *(.rodata .rodata.*)
    } :rodata

    .data : ALIGN(2M) {
        *(.data .data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    . = ALIGN(2M);
    PROVIDE(_kernel_end = .);

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}