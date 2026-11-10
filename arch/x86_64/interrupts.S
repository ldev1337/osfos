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
    # YMM upper halves stored at [r13 + 512 + (num * 16)]
    vextractf128 XMMWORD PTR [r13 + 512 + (\num * 16)], \reg, 1
.endm

.macro RESTORE_YMM_UPPER num, reg
    # Restore from the saved block pointer passed via rcx
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

    # R12 points at the saved registers on the per-CPU IST stack.
    mov r12, rsp

    # IST keeps the hardware entry frame off the interrupted stack. Preserve
    # the interrupted function's 128-byte red zone, then put its scheduler
    # frame and SIMD state below that area so the context survives a switch.
    mov r14, QWORD PTR [r12 + 160]
    lea rdi, [r14 - 440]
    lea rsi, [r14 - 128]
    mov rcx, 16
    rep movsq

    lea r13, [r14 - 1208]
    and r13, -64
    lea rdi, [r14 - 312]
    mov QWORD PTR [rdi], r13

    # Save SIMD state
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

    # Copy the normalized interrupt frame from the IST stack to the
    # interrupted thread's stack. There are 15 saved GPRs, vector/error, and
    # the five-word hardware return frame.
    lea rsi, [r12]
    lea rdi, [r14 - 304]
    mov rcx, 22
    rep movsq

    # Run C handlers on the IST stack. This avoids using a second thread's
    # stack while scheduling and keeps red-zone writes away from saved state.
    lea rsp, [r12 - 1024]
    and rsp, -16
    lea rdi, [r14 - 312]

    call c_interrupt_handler

    # Scheduler returns the selected thread's persistent context frame.
    mov rbx, rax

    # Load xstate pointer
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

    # Restore the selected thread's red zone before returning to its RSP.
    cmp QWORD PTR [rbx + 128], -1
    je .Lskip_redzone_restore
    mov rdx, QWORD PTR [rbx + 168]
    lea rdi, [rdx - 128]
    lea rsi, [rdx - 440]
    mov rcx, 16
    rep movsq
.Lskip_redzone_restore:

    # IRET must execute from IST. Copy the selected context to this CPU's
    # entry stack, then restore its GPRs and let IRET switch back to its RSP.
    lea rdi, [r12 - 2048]
    and rdi, -16
    mov rbp, rdi
    mov rsi, rbx
    mov rcx, 23
    rep movsq
    mov rsp, rbp

    # Skip xstate pointer.
    add rsp, 8

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

    # Skip vector and error code.
    add rsp, 16

    iretq

.size isr_common_stub, .-isr_common_stub

# Generate all 256 interrupt stubs.
.set i, 0
.rept 256
    .if (i == 8) || ((i >= 10) && (i <= 14)) || (i == 17) || (i == 30)
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
