
bits 32
default abs

section .multiboot_header
align 8
header_start:
    dd 0xE85250D6
    dd 0
    dd header_end - header_start
    dd -(0xE85250D6 + (header_end - header_start))
    dw 5
    dw 0
    dd 20
    dd 1024
    dd 768
    dd 32
    align 8
    dw 0
    dw 0
    dd 8
header_end:

section .bss
alignb 4096
global stack_bottom
global stack_top
stack_bottom:
    resb 16384
stack_top:

alignb 8
global multiboot_info_ptr
multiboot_info_ptr:
    resd 1

section .text
global start
extern main
extern setup_page_tables
extern enable_paging
extern gdt64_pointer
extern gdt64_code_segment

start:
    mov esp, stack_top

    cmp eax, 0x36D76289
    jne .no_multiboot

    mov [multiboot_info_ptr], ebx

    call setup_page_tables
    call enable_paging

    lgdt [gdt64_pointer]
    jmp 0x08:long_mode_start

.no_multiboot:
    mov al, 'M'
    mov [0xB8000], al
    cli
.no_multiboot_halt:
    hlt
    jmp .no_multiboot_halt

bits 64
long_mode_start:
    mov ax, 0x10
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rax, cr0
    and ax, 0xFFFB
    or ax, 0x0002
    mov cr0, rax

    mov rax, cr4
    or rax, (1 << 9)
    or rax, (1 << 10)
    or rax, (1 << 18)
    mov cr4, rax

    xor ecx, ecx
    xgetbv
    or eax, 7
    xsetbv

    mov edi, [multiboot_info_ptr]
    cld
    call main

.hang:
    cli
    hlt
    jmp .hang
