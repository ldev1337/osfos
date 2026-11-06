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

    # Preserve interrupted stack location.
    mov r12, rsp

    # Allocate stack frame:
    # - 512 bytes: fxsave64 storage
    # - 256 bytes: YMM upper halves storage (16 * 16)
    # Total = 768 bytes. No red-zone cushion needed: kernel is built with
    # -mno-red-zone, so the compiler never uses the 128 bytes below RSP.
    sub rsp, 768
    and rsp, -64

    # r13 points to the start of fxsave64 storage (base of our aligned block)
    mov r13, rsp

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

    # InterruptFrame begins with xstate pointer stored at [r12 - 8]
    mov QWORD PTR [r12 - 8], r13
    lea rdi, [r12 - 8]

    # Align stack to 16 bytes for the call (SysV ABI requirement).
    # With -mno-red-zone the compiler will not spill into memory below RSP,
    # so no cushion is needed here.
    mov rsp, r13
    and rsp, -16

    call c_interrupt_handler

    # Scheduler returns the selected context's frame pointer in rax.
    mov rsp, rax

    # Load xstate pointer
    mov rcx, QWORD PTR [rsp]
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