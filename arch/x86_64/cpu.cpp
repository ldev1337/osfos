#include <kernel/cpu.h>
#include <kernel/drivers/e1000.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/drivers/pit.h>
#include <kernel/scheduler.h>

struct GDTEntry {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_middle;
  uint8_t access;
  uint8_t granularity;
  uint8_t base_high;
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
  uint8_t ist;
  uint8_t type_attr;
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

extern "C" uint64_t c_interrupt_handler(struct InterruptFrame *frame) {
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

  if (vector == (0x20 + Kernel::Drivers::E1000::get_irq())) {
    Kernel::Drivers::E1000::handle_interrupt();
  }

  if (vector >= 0x20 && vector <= 0x2F) {
    if (vector >= 0x28)
      outb(0xA0, 0x20);

    outb(0x20, 0x20);
  }

  return next_rsp;
}

static void set_gdt_entry(int index, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran) {
  gdt_entries[index].limit_low = (limit & 0xFFFF);
  gdt_entries[index].base_low = (base & 0xFFFF);
  gdt_entries[index].base_middle = (base >> 16) & 0xFF;
  gdt_entries[index].access = access;
  gdt_entries[index].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
  gdt_entries[index].base_high = (base >> 24) & 0xFF;
}

static void set_idt_entry(int vector, uint64_t handler, uint16_t selector,
                          uint8_t type_attr) {
  idt_entries[vector].offset_low = (handler & 0xFFFF);
  idt_entries[vector].selector = selector;
  idt_entries[vector].ist = 0;
  idt_entries[vector].type_attr = type_attr;
  idt_entries[vector].offset_middle = (handler >> 16) & 0xFFFF;
  idt_entries[vector].offset_high = (handler >> 32) & 0xFFFF;
  idt_entries[vector].zero = 0;
}

static void gdt_flush(uint64_t gdt_ptr_addr) {
  __asm__ volatile("lgdt (%0)\n\t"
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
                   :
                   : "r"(gdt_ptr_addr)
                   : "rax", "memory");
}

__attribute__((always_inline)) static void idt_flush(uint64_t idt_ptr_addr) {
  __asm__ volatile("lidt (%0)" : : "r"(idt_ptr_addr) : "memory");
}

static void init_gdt(void) {
  gdt_ptr.limit = sizeof(gdt_entries) - 1;
  gdt_ptr.base = (uint64_t)(&gdt_entries);

  set_gdt_entry(0, 0, 0, 0, 0);             // Null descriptor (0x00)
  set_gdt_entry(1, 0, 0xFFFFF, 0x9A, 0x20); // Ring 0 code (0x08)
  set_gdt_entry(2, 0, 0xFFFFF, 0x92, 0x00); // Ring 0 data (0x10)
  set_gdt_entry(3, 0, 0xFFFFF, 0xF2, 0x00); // Ring 3 data (0x1B)
  set_gdt_entry(4, 0, 0xFFFFF, 0xFA, 0x20); // Ring 3 code (0x23)
  set_gdt_entry(5, 0, 0xFFFFF, 0xBA, 0x20); // Ring 1 code (0x29)
  set_gdt_entry(6, 0, 0xFFFFF, 0xB2, 0x00); // Ring 1 data (0x31)

  gdt_flush((uint64_t)(&gdt_ptr));
}

static void init_idt(void) {
  idt_ptr.limit = sizeof(idt_entries) - 1;
  idt_ptr.base = (uint64_t)(&idt_entries);

  for (int i = 0; i < 256; ++i) {
    set_idt_entry(i, isr_stub_table[i], 0x08, 0x8E);
  }

  idt_flush((uint64_t)(&idt_ptr));
}

__attribute__((always_inline)) static void io_wait(void) { outb(0x80, 0); }

static void init_pic(void) {
  outb(0x21, 0xFF);
  outb(0xA1, 0xFF);

  outb(0x20, 0x11);
  io_wait();
  outb(0xA0, 0x11);
  io_wait();

  outb(0x21, 0x20);
  io_wait();
  outb(0xA1, 0x28);
  io_wait();

  outb(0x21, 0x04);
  io_wait();
  outb(0xA1, 0x02);
  io_wait();

  outb(0x21, 0x01);
  io_wait();
  outb(0xA1, 0x01);
  io_wait();

  outb(0x21, 0xF8);
  outb(0xA1, 0xEF);
}

static bool cpuid_supported(uint32_t leaf, uint32_t subleaf, uint32_t *eax,
                            uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
  __asm__ volatile("cpuid"
                   : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                   : "a"(leaf), "c"(subleaf));
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

  const uint32_t CPUID_XSAVE = 1U << 26;
  const uint32_t CPUID_OSXSAVE = 1U << 27;
  const uint32_t CPUID_AVX = 1U << 28;

  if ((ecx & (CPUID_XSAVE | CPUID_OSXSAVE | CPUID_AVX)) !=
      (CPUID_XSAVE | CPUID_OSXSAVE | CPUID_AVX)) {
    return false;
  }

  cpuid_supported(7, 0, &eax, &ebx, &ecx, &edx);

  const uint32_t CPUID_AVX2 = 1U << 5;

  return (ebx & CPUID_AVX2) != 0;
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
