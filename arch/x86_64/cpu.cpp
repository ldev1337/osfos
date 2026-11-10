#include <kernel/cpu.h>
#include <kernel/drivers/e1000.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/drivers/pit.h>
#include <kernel/init.h>
#include <kernel/scheduler.h>
#include <kernel/smp.h>

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

namespace {

constexpr size_t INTERRUPT_STACK_SIZE = 16 * 1024;
constexpr unsigned INTERRUPT_STACK_COUNT = 4;
constexpr uint16_t TSS_SELECTOR = 0x38;

struct Tss64 {
  uint32_t reserved0;
  uint64_t rsp[3];
  uint64_t reserved1;
  uint64_t ist[7];
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t io_map_base;
} __attribute__((packed));

static_assert(sizeof(Tss64) == 104);

struct CpuTables {
  GDTEntry gdt[7];
  uint64_t tss_descriptor[2];
  Tss64 tss;
  alignas(16) uint8_t interrupt_stacks[INTERRUPT_STACK_COUNT]
                                       [INTERRUPT_STACK_SIZE];
  GDTPtr gdtr;
};

alignas(64) static CpuTables cpu_tables[Kernel::SMP::MAX_CPUS];

} // namespace

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
static uint8_t pic_master_mask = 0xFF;
static uint8_t pic_slave_mask = 0xFF;

struct InterruptFrame {
  uint64_t xstate_ptr;
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
  uint64_t interrupt_number;
  uint64_t error_code;
  uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

static_assert(sizeof(InterruptFrame) == 184);

extern uint64_t isr_stub_table[];
extern "C" void ata_irq_handler();

extern "C" uint64_t c_interrupt_handler(struct InterruptFrame *frame) {
  const uint64_t vector = frame->interrupt_number;
  uint64_t next_rsp = (uint64_t)frame;

  if (vector == 0x20) {
    Kernel::Drivers::PIT::tick();
    if (Kernel::Scheduler::should_preempt())
      next_rsp = Kernel::Scheduler::schedule((uint64_t)frame);
  } else if (vector == 0x30) {
    next_rsp = Kernel::Scheduler::schedule((uint64_t)frame);
  } else if (vector == 0x31) {
    Kernel::SMP::local_apic_eoi();
    next_rsp = Kernel::Scheduler::schedule((uint64_t)frame);
  }

  if (vector == 0x21) {
    Kernel::Drivers::Keyboard::handle_interrupt();
  }

  if (vector == 0x2C) {
    Kernel::Drivers::Mouse::handle_interrupt();
  }

  if (vector == 0x2E) {
    ata_irq_handler();
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

static void set_gdt_entry(GDTEntry *entries, int index, uint32_t base,
                          uint32_t limit,
                          uint8_t access, uint8_t gran) {
  entries[index].limit_low = (limit & 0xFFFF);
  entries[index].base_low = (base & 0xFFFF);
  entries[index].base_middle = (base >> 16) & 0xFF;
  entries[index].access = access;
  entries[index].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
  entries[index].base_high = (base >> 24) & 0xFF;
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
  const uint16_t selector = TSS_SELECTOR;
  __asm__ volatile("ltr %0" : : "rm"(selector) : "memory");
}

__attribute__((always_inline)) static void idt_flush(uint64_t idt_ptr_addr) {
  __asm__ volatile("lidt (%0)" : : "r"(idt_ptr_addr) : "memory");
}

static bool init_gdt(unsigned cpu_index) {
  if (cpu_index >= Kernel::SMP::MAX_CPUS)
    return false;

  CpuTables &tables = cpu_tables[cpu_index];
  tables.gdtr.limit = sizeof(tables.gdt) + sizeof(tables.tss_descriptor) - 1;
  tables.gdtr.base = reinterpret_cast<uint64_t>(tables.gdt);

  set_gdt_entry(tables.gdt, 0, 0, 0, 0, 0);
  set_gdt_entry(tables.gdt, 1, 0, 0xFFFFF, 0x9A, 0x20);
  set_gdt_entry(tables.gdt, 2, 0, 0xFFFFF, 0x92, 0x00);
  set_gdt_entry(tables.gdt, 3, 0, 0xFFFFF, 0xF2, 0x00);
  set_gdt_entry(tables.gdt, 4, 0, 0xFFFFF, 0xFA, 0x20);
  set_gdt_entry(tables.gdt, 5, 0, 0xFFFFF, 0xBA, 0x20);
  set_gdt_entry(tables.gdt, 6, 0, 0xFFFFF, 0xB2, 0x00);

  tables.tss = {};
  for (unsigned i = 0; i < INTERRUPT_STACK_COUNT; ++i) {
    tables.tss.ist[i] = reinterpret_cast<uint64_t>(
        tables.interrupt_stacks[i] + INTERRUPT_STACK_SIZE);
  }
  tables.tss.io_map_base = sizeof(Tss64);
  const uint64_t base = reinterpret_cast<uint64_t>(&tables.tss);
  const uint64_t limit = sizeof(Tss64) - 1;
  tables.tss_descriptor[0] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) |
                             (uint64_t(0x89) << 40) |
                             ((limit & 0xF0000) << 32) |
                             ((base & 0xFF000000) << 32);
  tables.tss_descriptor[1] = base >> 32;
  gdt_flush(reinterpret_cast<uint64_t>(&tables.gdtr));
  return true;
}

static void __init init_idt(void) {
  idt_ptr.limit = sizeof(idt_entries) - 1;
  idt_ptr.base = (uint64_t)(&idt_entries);

  for (int i = 0; i < 256; ++i) {
    set_idt_entry(i, isr_stub_table[i], 0x08, 0x8E);
    const uint8_t ist = (i == 8) ? 3 : (i == 2) ? 4 : (i == 18) ? 3
                              : (i < 32)        ? 2
                                                : 1;
    idt_entries[i].ist = ist;
  }

  idt_flush((uint64_t)(&idt_ptr));
}

__attribute__((always_inline)) static void io_wait(void) { outb(0x80, 0); }

static void __init init_pic(void) {
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

  pic_master_mask = 0xF8;
  pic_slave_mask = 0xEF;
  outb(0x21, pic_master_mask);
  outb(0xA1, pic_slave_mask);
}

extern "C" bool cpu_pic_unmask_irq(uint8_t irq) {
  if (irq >= 16)
    return false;

  if (irq < 8) {
    pic_master_mask &= static_cast<uint8_t>(~(1u << irq));
    outb(0x21, pic_master_mask);
    return true;
  }

  pic_slave_mask &= static_cast<uint8_t>(~(1u << (irq - 8)));
  pic_master_mask &= static_cast<uint8_t>(~(1u << 2));
  outb(0xA1, pic_slave_mask);
  outb(0x21, pic_master_mask);
  return true;
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

__init void cpu_init(void) {
  if (!init_gdt(0)) {
    __asm__ volatile("cli");
    while (1)
      __asm__ volatile("hlt");
  }
  init_idt();
  init_pic();

  if (!verify_avx2_support()) {
    __asm__ volatile("cli");
    while (1) {
      __asm__ volatile("hlt");
    }
  }
}

extern "C" void cpu_ap_init(uint32_t cpu_index) {
  if (!init_gdt(cpu_index)) {
    __asm__ volatile("cli");
    for (;;)
      __asm__ volatile("hlt");
  }
  idt_flush(reinterpret_cast<uint64_t>(&idt_ptr));
}
