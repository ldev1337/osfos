bits 16
default abs

section .os_init
align 16
global ap_trampoline_start
global ap_trampoline_end
global ap_trampoline_cr3
global ap_trampoline_stack
global ap_trampoline_entry

ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00
    lgdt [0x8000 + (ap_gdt_descriptor - ap_trampoline_start)]

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    mov eax, [0x8000 + (ap_trampoline_cr3 - ap_trampoline_start)]
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | 1
    mov cr0, eax
    jmp dword 0x08:(0x8000 + (ap_long_mode - ap_trampoline_start))

bits 64
ap_long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rsp, [abs 0x8000 + (ap_trampoline_stack - ap_trampoline_start)]
    and rsp, -16
    xor rbp, rbp
    mov rax, [abs 0x8000 + (ap_trampoline_entry - ap_trampoline_start)]
    call rax
.halt:
    cli
    hlt
    jmp .halt

align 8
ap_gdt:
    dq 0
    dq 0x00AF9A000000FFFF
    dq 0x00CF92000000FFFF
ap_gdt_end:
ap_gdt_descriptor:
    dw ap_gdt_end - ap_gdt - 1
    dd 0x8000 + (ap_gdt - ap_trampoline_start)

align 8
ap_trampoline_cr3:
    dd 0
ap_trampoline_stack:
    dq 0
ap_trampoline_entry:
    dq 0
ap_trampoline_end:
