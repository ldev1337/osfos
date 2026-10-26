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

    mov rdi, rsp
    and rsp, -16         ; Interrupts can arrive with arbitrary stack alignment.
    call c_interrupt_handler
    mov rsp, rax          ; Switch to the new thread's stack pointer returned by scheduler

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
