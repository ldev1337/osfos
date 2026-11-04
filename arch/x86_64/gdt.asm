
bits 32
default abs

section .rodata
align 8
gdt64:
    dq 0

gdt64_code_segment equ 0x08
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)

gdt64_data_segment equ 0x10
    dq (1 << 44) | (1 << 47) | (1 << 41)

global gdt64_pointer
gdt64_pointer:
    dw $ - gdt64 - 1
    dd gdt64
