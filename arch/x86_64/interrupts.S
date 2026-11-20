.intel_syntax noprefix
.code64
.altmacro

.section .text
.extern c_interrupt_handler
.global isr_common_stub
.type isr_common_stub, @function

.macro ISR_NO_ERRCODE num
.global isr\num
isr\num:
    push 0
    push \num
    jmp isr_common_stub
.endm

.macro ISR_ERRCODE num
.global isr\num
isr\num:
    push \num
    jmp isr_common_stub
.endm

.macro SAVE_YMM_UPPER num, reg
    vextractf128 XMMWORD PTR [r13 + 512 + (\num * 16)], \reg, 1
.endm

.macro RESTORE_YMM_UPPER num, reg
    vinsertf128 \reg, \reg, XMMWORD PTR [rcx + 512 + (\num * 16)], 1
.endm

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

    mov r12, rsp
    mov r14, QWORD PTR [r12 + 160]

    # Persistent ThreadContext: xstate pointer at old_rsp-312, then the
    # normalized 22-qword interrupt frame at old_rsp-304.
    lea r13, [r14 - 1208]
    and r13, -64
    lea rdi, [r14 - 312]
    mov QWORD PTR [rdi], r13

    # Save x87/SSE state and the upper 128 bits of YMM0-YMM15.
    fxsave64 [r13]
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

    # Interrupt entry does not clear DF. The kernel ABI requires DF=0, and
    # both REP MOVSQ operations below depend on forward copying.
    cld
    lea rsi, [r12]
    lea rdi, [r14 - 304]
    mov ecx, 22
    rep movsq

    # Run C on the IST stack, not the interrupted thread's red-zone area.
    lea rsp, [r12 - 1024]
    and rsp, -16
    lea rdi, [r14 - 312]
    vzeroupper
    call c_interrupt_handler

    # RAX is the selected thread's persistent ThreadContext.
    mov rbx, rax
    mov rcx, QWORD PTR [rbx]
    fxrstor64 [rcx]
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

    # The xstate pointer is not part of the IRET frame. Copy only the 22
    # qwords beginning at r15, saving one qword of traffic on every handoff.
    lea rsi, [rbx + 8]
    lea rdi, [r12 - 2040]
    and rdi, -16
    mov rsp, rdi
    mov ecx, 22
    rep movsq

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

.size isr_common_stub, .-isr_common_stub

# CPU exceptions that push an error code: 8, 10-14, 17, 21, 29, 30.
# Vector 48 (INT 0x30, scheduler yield) correctly uses the no-error stub.
.set i, 0
.rept 256
    .if (i == 8) || ((i >= 10) && (i <= 14)) || (i == 17) || (i == 21) || (i == 29) || (i == 30)
        ISR_ERRCODE %i
    .else
        ISR_NO_ERRCODE %i
    .endif
    .set i, i + 1
.endr

.section .data
.p2align 3
.global isr_stub_table
.type isr_stub_table, @object
isr_stub_table:
.macro ISR_TABLE_ENTRY num
    .quad isr\num
.endm
.set i, 0
.rept 256
    ISR_TABLE_ENTRY %i
    .set i, i + 1
.endr
.size isr_stub_table, .-isr_stub_table

.section .note.GNU-stack,"",@progbits
