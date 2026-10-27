; /arch/x86_64/interrupts.asm
bits 64
section .text

%macro ISR_NO_ERRCODE 1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
isr%1:
    push qword %1
    jmp isr_common_stub
%endmacro

extern c_interrupt_handler
global isr_common_stub

isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Keep each interrupted context's SIMD/AVX state on its own stack so a
    ; suspended thread retains it, and nested interrupts get separate storage.
    mov r12, rsp
    sub rsp, 896
    and rsp, -64
    fxsave64 [rsp]
%macro SAVE_YMM_UPPER 2
    vextractf128 oword [rsp + 512 + (%1 * 16)], %2, 1
%endmacro
    SAVE_YMM_UPPER 0, ymm0
    SAVE_YMM_UPPER 1, ymm1
    SAVE_YMM_UPPER 2, ymm2
    SAVE_YMM_UPPER 3, ymm3
    SAVE_YMM_UPPER 4, ymm4
    SAVE_YMM_UPPER 5, ymm5
    SAVE_YMM_UPPER 6, ymm6
    SAVE_YMM_UPPER 7, ymm7
    SAVE_YMM_UPPER 8, ymm8
    SAVE_YMM_UPPER 9, ymm9
    SAVE_YMM_UPPER 10, ymm10
    SAVE_YMM_UPPER 11, ymm11
    SAVE_YMM_UPPER 12, ymm12
    SAVE_YMM_UPPER 13, ymm13
    SAVE_YMM_UPPER 14, ymm14
    SAVE_YMM_UPPER 15, ymm15
    mov [r12 - 8], rsp    ; xstate pointer is the first InterruptFrame field.
    lea rdi, [r12 - 8]
    and rsp, -16          ; Interrupts can arrive with arbitrary stack alignment.
    call c_interrupt_handler
    mov rsp, rax          ; Switch to the new thread's stack pointer returned by scheduler

    mov rcx, [rsp]
    fxrstor64 [rcx]
%macro RESTORE_YMM_UPPER 2
    vinsertf128 %2, %2, oword [rcx + 512 + (%1 * 16)], 1
%endmacro
    RESTORE_YMM_UPPER 0, ymm0
    RESTORE_YMM_UPPER 1, ymm1
    RESTORE_YMM_UPPER 2, ymm2
    RESTORE_YMM_UPPER 3, ymm3
    RESTORE_YMM_UPPER 4, ymm4
    RESTORE_YMM_UPPER 5, ymm5
    RESTORE_YMM_UPPER 6, ymm6
    RESTORE_YMM_UPPER 7, ymm7
    RESTORE_YMM_UPPER 8, ymm8
    RESTORE_YMM_UPPER 9, ymm9
    RESTORE_YMM_UPPER 10, ymm10
    RESTORE_YMM_UPPER 11, ymm11
    RESTORE_YMM_UPPER 12, ymm12
    RESTORE_YMM_UPPER 13, ymm13
    RESTORE_YMM_UPPER 14, ymm14
    RESTORE_YMM_UPPER 15, ymm15
    add rsp, 8            ; Skip xstate_ptr before restoring general registers.

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16

    iretq

%assign i 0
%rep 256
    %if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 30
        ISR_ERRCODE i
    %else
        ISR_NO_ERRCODE i
    %endif
    %assign i i + 1
%endrep

section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr%+i
    %assign i i + 1
%endrep
