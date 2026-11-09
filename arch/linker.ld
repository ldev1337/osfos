ENTRY(start)

PHDRS {
    text   PT_LOAD FLAGS(5);  /* R+X: text + rodata + init share one huge page */
    data   PT_LOAD FLAGS(6);  /* R+W */
}

SECTIONS {
    . = 2M;

    .text : {
        KEEP(*(.multiboot_header))
        *(.text .text.*)
    } :text

    .rodata : ALIGN(64) {
        *(.rodata .rodata.*)
    } :text                      /* same R+X huge page, no 2M gap */

    /* Reclaimable boot code: lives in the RX page, freed after boot
       only if you split the huge page or just reuse the frames in your
       direct map. */
    .os_init : ALIGN(4K) {
        _os_init_start = .;
        *(.os_init)
        *(.os_initdata)
        . = ALIGN(4K);
        _os_init_end = .;
    } :text

    .data : ALIGN(2M) {          /* the only 2M boundary: W^X split */
        *(.data .data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /* Real end of the image. Frames from here up to _kernel_end_2m are
       already covered by the RW huge page, so hand them to the
       allocator instead of wasting them. */
    . = ALIGN(4K);
    PROVIDE(_kernel_end = .);
    PROVIDE(_kernel_end_2m = ALIGN(2M));

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}