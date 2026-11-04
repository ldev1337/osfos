
bits 32
default abs

section .bss
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
global setup_page_tables
global enable_paging

setup_page_tables:
    ; PML4[0] -> PDPT
    mov eax, pdpt_table
    or eax, 3
    mov [pml4_table], eax

    ; PDPT[0..3] -> page directories
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

    ; Map 4 GiB using 2 MiB pages
    mov edi, pd_table
    xor esi, esi
    mov edx, 4

.map_gib:
    xor ecx, ecx

.map_2m:
    mov eax, ecx
    shl eax, 21
    add eax, esi
    or eax, 0x83
    mov [edi + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jl .map_2m

    add esi, 0x40000000
    add edi, 4096
    dec edx
    jnz .map_gib

    ret

enable_paging:
    mov eax, pml4_table
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 0)
    mov cr0, eax

    ret
