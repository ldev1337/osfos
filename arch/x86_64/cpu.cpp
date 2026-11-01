#include <kernel/cpu.h>
#include <kernel/scheduler.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/drivers/pit.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <x86intrin.h>

extern "C" void kernel_printf(const char* fmt, ...);

struct GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct GDTPtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct GDTEntry gdt_entries[7];
static struct GDTPtr gdt_ptr;

struct IDTEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct IDTPtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct IDTEntry idt_entries[256];
static struct IDTPtr idt_ptr;

struct InterruptFrame {
    uint64_t xstate_ptr;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t interrupt_number;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

extern uint64_t isr_stub_table[];

extern "C" uint64_t c_interrupt_handler(struct InterruptFrame* frame) {
    const uint64_t vector = frame->interrupt_number;
    uint64_t next_rsp = (uint64_t)frame;

    if (vector == 0x20) {
        Kernel::Drivers::PIT::tick();
        if (Kernel::Scheduler::should_preempt())
            next_rsp = Kernel::Scheduler::schedule((uint64_t)frame);
    } else if (vector == 0x30) {
        next_rsp = Kernel::Scheduler::schedule((uint64_t)frame);
    }

    if (vector == 0x21) {
        Kernel::Drivers::Keyboard::handle_interrupt();
    }

    if (vector == 0x2C) {
        Kernel::Drivers::Mouse::handle_interrupt();
    }

    if (vector >= 0x20 && vector <= 0x2F) {
        if (vector >= 0x28)
            outb(0xA0, 0x20);

        outb(0x20, 0x20);
    }

    return next_rsp;
}

static void set_gdt_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[index].limit_low     = (limit & 0xFFFF);
    gdt_entries[index].base_low      = (base & 0xFFFF);
    gdt_entries[index].base_middle   = (base >> 16) & 0xFF;
    gdt_entries[index].access        = access;
    gdt_entries[index].granularity   = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt_entries[index].base_high     = (base >> 24) & 0xFF;
}

static void set_idt_entry(int vector, uint64_t handler, uint16_t selector, uint8_t type_attr) {
    idt_entries[vector].offset_low     = (handler & 0xFFFF);
    idt_entries[vector].selector       = selector;
    idt_entries[vector].ist            = 0;
    idt_entries[vector].type_attr      = type_attr;
    idt_entries[vector].offset_middle  = (handler >> 16) & 0xFFFF;
    idt_entries[vector].offset_high    = (handler >> 32) & 0xFFFF;
    idt_entries[vector].zero           = 0;
}

static void gdt_flush(uint64_t gdt_ptr_addr) {
    __asm__ volatile(
        "lgdt (%0)\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        : : "r"(gdt_ptr_addr) : "rax", "memory"
    );
}

__attribute__((always_inline)) static void idt_flush(uint64_t idt_ptr_addr) {
    __asm__ volatile("lidt (%0)" : : "r"(idt_ptr_addr) : "memory");
}

static void init_gdt(void) {
    gdt_ptr.limit = sizeof(gdt_entries) - 1;
    gdt_ptr.base  = (uint64_t)(&gdt_entries);

    set_gdt_entry(0, 0, 0, 0, 0);                 // Null descriptor (0x00)
    set_gdt_entry(1, 0, 0xFFFFF, 0x9A, 0x20);     // Ring 0 code (0x08)
    set_gdt_entry(2, 0, 0xFFFFF, 0x92, 0x00);     // Ring 0 data (0x10)
    set_gdt_entry(3, 0, 0xFFFFF, 0xF2, 0x00);     // Ring 3 data (0x1B)
    set_gdt_entry(4, 0, 0xFFFFF, 0xFA, 0x20);     // Ring 3 code (0x23)
    set_gdt_entry(5, 0, 0xFFFFF, 0xBA, 0x20);     // Ring 1 code (0x29)
    set_gdt_entry(6, 0, 0xFFFFF, 0xB2, 0x00);     // Ring 1 data (0x31)

    gdt_flush((uint64_t)(&gdt_ptr));
}

static void init_idt(void) {
    idt_ptr.limit = sizeof(idt_entries) - 1;
    idt_ptr.base  = (uint64_t)(&idt_entries);

    for (int i = 0; i < 256; ++i) {
        set_idt_entry(i, isr_stub_table[i], 0x08, 0x8E);
    }

    idt_flush((uint64_t)(&idt_ptr));
}

__attribute__((always_inline)) static void io_wait(void) {
    outb(0x80, 0);
}

static void init_pic(void) {
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();

    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();

    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();

    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();

    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
}

static bool cpuid_supported(uint32_t leaf, uint32_t subleaf,
                            uint32_t* eax, uint32_t* ebx,
                            uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
    );
    return true;
}

static bool verify_avx2_support(void) {
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0), "c"(0));

    if (eax < 7)
        return false;

    cpuid_supported(1, 0, &eax, &ebx, &ecx, &edx);

    const uint32_t CPUID_XSAVE   = 1u << 26;
    const uint32_t CPUID_OSXSAVE = 1u << 27;
    const uint32_t CPUID_AVX     = 1u << 28;

    if ((ecx & (CPUID_XSAVE | CPUID_OSXSAVE | CPUID_AVX)) !=
        (CPUID_XSAVE | CPUID_OSXSAVE | CPUID_AVX)) {
        return false;
    }

    cpuid_supported(7, 0, &eax, &ebx, &ecx, &edx);

    const uint32_t CPUID_AVX2 = 1u << 5;

    if (!(ebx & CPUID_AVX2))
        return false;

    return true;
}

void cpu_init(void) {
    init_gdt();
    init_idt();
    init_pic();

    if (!verify_avx2_support()) {
        __asm__ volatile("cli");
        while (1) {
            __asm__ volatile("hlt");
        }
    }
}

#define KEEP_MEMORY_BARRIER() __asm__ volatile("" ::: "memory")

__attribute__((noinline, target("sse2,no-avx")))
static void test_routine_sse2_context_switch(uint8_t* save_area, size_t iterations) {
    for (size_t i = 0; i < iterations; ++i) {
        __asm__ volatile(
            "movaps %%xmm0, 0(%0)\n\t"   "movaps %%xmm1, 16(%0)\n\t"
            "movaps %%xmm2, 32(%0)\n\t"  "movaps %%xmm3, 48(%0)\n\t"
            "movaps %%xmm4, 64(%0)\n\t"  "movaps %%xmm5, 80(%0)\n\t"
            "movaps %%xmm6, 96(%0)\n\t"  "movaps %%xmm7, 112(%0)\n\t"
            "movaps %%xmm8, 128(%0)\n\t" "movaps %%xmm9, 144(%0)\n\t"
            "movaps %%xmm10, 160(%0)\n\t""movaps %%xmm11, 176(%0)\n\t"
            "movaps %%xmm12, 192(%0)\n\t""movaps %%xmm13, 208(%0)\n\t"
            "movaps %%xmm14, 224(%0)\n\t""movaps %%xmm15, 240(%0)\n\t"

            "movaps 0(%0), %%xmm0\n\t"   "movaps 16(%0), %%xmm1\n\t"
            "movaps 32(%0), %%xmm2\n\t"  "movaps 48(%0), %%xmm3\n\t"
            "movaps 64(%0), %%xmm4\n\t"  "movaps 80(%0), %%xmm5\n\t"
            "movaps 96(%0), %%xmm6\n\t"  "movaps 112(%0), %%xmm7\n\t"
            "movaps 128(%0), %%xmm8\n\t" "movaps 144(%0), %%xmm9\n\t"
            "movaps 160(%0), %%xmm10\n\t""movaps 176(%0), %%xmm11\n\t"
            "movaps 192(%0), %%xmm12\n\t""movaps 208(%0), %%xmm13\n\t"
            "movaps 224(%0), %%xmm14\n\t""movaps 240(%0), %%xmm15\n\t"
            : : "r"(save_area) : "memory",
              "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
              "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
        );
    }
    KEEP_MEMORY_BARRIER();
}

__attribute__((noinline, target("avx2")))
static void test_routine_avx2_context_switch(uint8_t* save_area, size_t iterations) {
    for (size_t i = 0; i < iterations; ++i) {
        __asm__ volatile(
            "vmovaps %%ymm0, 0(%0)\n\t"   "vmovaps %%ymm1, 32(%0)\n\t"
            "vmovaps %%ymm2, 64(%0)\n\t"  "vmovaps %%ymm3, 96(%0)\n\t"
            "vmovaps %%ymm4, 128(%0)\n\t" "vmovaps %%ymm5, 160(%0)\n\t"
            "vmovaps %%ymm6, 192(%0)\n\t" "vmovaps %%ymm7, 224(%0)\n\t"
            "vmovaps %%ymm8, 256(%0)\n\t" "vmovaps %%ymm9, 288(%0)\n\t"
            "vmovaps %%ymm10, 320(%0)\n\t""vmovaps %%ymm11, 352(%0)\n\t"
            "vmovaps %%ymm12, 384(%0)\n\t""vmovaps %%ymm13, 416(%0)\n\t"
            "vmovaps %%ymm14, 448(%0)\n\t""vmovaps %%ymm15, 480(%0)\n\t"

            "vmovaps 0(%0), %%ymm0\n\t"   "vmovaps 32(%0), %%ymm1\n\t"
            "vmovaps 64(%0), %%ymm2\n\t"  "vmovaps 96(%0), %%ymm3\n\t"
            "vmovaps 128(%0), %%ymm4\n\t" "vmovaps 160(%0), %%ymm5\n\t"
            "vmovaps 192(%0), %%ymm6\n\t" "vmovaps 224(%0), %%ymm7\n\t"
            "vmovaps 256(%0), %%ymm8\n\t" "vmovaps 288(%0), %%ymm9\n\t"
            "vmovaps 320(%0), %%ymm10\n\t""vmovaps 352(%0), %%ymm11\n\t"
            "vmovaps 384(%0), %%ymm12\n\t""vmovaps 416(%0), %%ymm13\n\t"
            "vmovaps 448(%0), %%ymm14\n\t""vmovaps 480(%0), %%ymm15\n\t"
            : : "r"(save_area) : "memory",
              "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",
              "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15"
        );
    }
    KEEP_MEMORY_BARRIER();
}

void run_vector_benchmark(void) {
    _Alignas(64) uint8_t tcb_save_buffer[512];
    const size_t SWITCH_ITERATIONS = 100000;
    unsigned int aux;

    uint64_t start_sse2 = __rdtscp(&aux);
    test_routine_sse2_context_switch(tcb_save_buffer, SWITCH_ITERATIONS);
    uint64_t total_cycles_sse2 = __rdtscp(&aux) - start_sse2;

    uint64_t start_avx2 = __rdtscp(&aux);
    test_routine_avx2_context_switch(tcb_save_buffer, SWITCH_ITERATIONS);
    uint64_t total_cycles_avx2 = __rdtscp(&aux) - start_avx2;

    uint64_t avg_sse2 = total_cycles_sse2 / SWITCH_ITERATIONS;
    uint64_t avg_avx2 = total_cycles_avx2 / SWITCH_ITERATIONS;

    kernel_printf("=== CONTEXT SWITCH COST BENCHMARK ===\n");
    kernel_printf("Iterations: %zu switches\n", SWITCH_ITERATIONS);
    kernel_printf("SSE2 (XMM 256B) -> Avg: %llu cycles/switch | Total: %llu cycles\n", avg_sse2, total_cycles_sse2);
    kernel_printf("AVX2 (YMM 512B) -> Avg: %llu cycles/switch | Total: %llu cycles\n", avg_avx2, total_cycles_avx2);
}
