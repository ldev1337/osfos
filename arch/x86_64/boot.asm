; /arch/x86_64/boot.asm
bits 32
default abs

section .multiboot_header
align 8

header_start:
    dd 0xE85250D6
    dd 0
    dd header_end - header_start
    dd -(0xE85250D6 + (header_end - header_start))

    ; Multiboot2 framebuffer request
    dw 5
    dw 0
    dd 20
    dd 1024
    dd 768
    dd 32

    align 8

    ; End tag
    dw 0
    dw 0
    dd 8

header_end:

section .bss
alignb 4096
stack_bottom:
    resb 16384
stack_top:

; Preserve the Multiboot2 information pointer
align 8
multiboot_info_ptr:
    resd 1

; Page tables: identity-map the first 4 GiB
alignb 4096
pml4_table:
    resb 4096
pdpt_table:
    resb 4096
pd_table:
    resb 4096
pd1_table:
    resb 4096
pd2_table:
    resb 4096
pd3_table:
    resb 4096

section .text
global start
extern main

start:
    mov esp, stack_top

    ; Verify Multiboot2 bootloader magic
    cmp eax, 0x36D76289
    jne .no_multiboot

    ; Preserve EBX before page-table setup clobbers it
    mov [multiboot_info_ptr], ebx

    ; Set up paging and enter long mode
    call setup_page_tables
    call enable_paging

    ; Load the 64-bit GDT
    lgdt [gdt64.pointer]

    ; Enter the 64-bit code segment (selector 0x08)
    jmp gdt64.code_segment:long_mode_start

.no_multiboot:
    mov al, 'M'
    mov [0xB8000], al
    cli
.no_multiboot_halt:
    hlt
    jmp .no_multiboot_halt

; Map physical 0x00000000-0xFFFFFFFF using 2 MiB pages
setup_page_tables:
    ; PML4[0] -> PDPT
    mov eax, pdpt_table
    or eax, 3
    mov [pml4_table], eax

    ; PDPT[0..3] -> four page directories
    mov eax, pd_table
    or eax, 3
    mov [pdpt_table], eax

    mov eax, pd1_table
    or eax, 3
    mov [pdpt_table + 8], eax

    mov eax, pd2_table
    or eax, 3
    mov [pdpt_table + 16], eax

    mov eax, pd3_table
    or eax, 3
    mov [pdpt_table + 24], eax

    ; Fill all four page directories
    mov edi, pd_table
    xor esi, esi
    mov edx, 4

.map_gib:
    xor ecx, ecx

.map_2m:
    mov eax, ecx
    shl eax, 21
    add eax, esi
    or eax, 0x83                ; Present, writable, huge page
    mov [edi + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jl .map_2m

    add esi, 0x40000000         ; Next 1 GiB
    add edi, 4096
    dec edx
    jnz .map_gib

    ret

enable_paging:
    ; Load PML4 into CR3
    mov eax, pml4_table
    mov cr3, eax

    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Enable long mode in EFER
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable paging and protected mode
    mov eax, cr0
    or eax, (1 << 31) | (1 << 0)
    mov cr0, eax

    ret

bits 64
long_mode_start:
    ; Load valid data-segment selectors
    mov ax, 0x10
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rax, cr0
    and ax, 0xFFFB      ; Clear CR0.EM (coprocessor emulation)
    or ax, 0x0002       ; Set CR0.MP (monitor coprocessor)
    mov cr0, rax

    mov rax, cr4
    or rax, (1 << 9)    ; OSFXSR
    or rax, (1 << 10)   ; OSXMMEXCPT
    or rax, (1 << 18)   ; OSXSAVE
    mov cr4, rax

    xor rcx, rcx        ; XCR0 index 0
    xgetbv
    or rax, 3           ; x87 + SSE
    or rax, 4           ; AVX state
    xsetbv

    mov edi, [multiboot_info_ptr]
    cld
    call main

.hang:
    cli
    hlt
    jmp .hang

section .rodata
align 8
gdt64:
    dq 0                        ; Null descriptor

.code_segment: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)

.data_segment: equ $ - gdt64
    dq (1 << 44) | (1 << 47) | (1 << 41)

.pointer:
    dw $ - gdt64 - 1
    dq gdt64