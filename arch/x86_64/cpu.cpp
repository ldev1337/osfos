#include <kernel/cpu.h>
#include <kernel/drivers/e1000.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/drivers/tsc_deadline.h>
#include <kernel/init.h>
#include <kernel/scheduler.h>
#include <kernel/smp.h>

#include <cstddef>
#include <cstdint>

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
constexpr uint16_t KERNEL_CODE_SELECTOR = 0x08;
constexpr uint8_t IDT_INTERRUPT_GATE = 0x8E;

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

struct alignas(64) CpuTables {
  GDTEntry gdt[7];
  uint64_t tss_descriptor[2];
  Tss64 tss;
  alignas(16) uint8_t
      interrupt_stacks[INTERRUPT_STACK_COUNT][INTERRUPT_STACK_SIZE];
  GDTPtr gdtr;
};

static_assert(alignof(CpuTables) == 64);
static_assert(sizeof(CpuTables) % 64 == 0);

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

static_assert(sizeof(IDTEntry) == 16);
static_assert(sizeof(IDTPtr) == 10);

static IDTEntry idt_entries[256];
static IDTPtr idt_ptr;
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
static_assert(offsetof(InterruptFrame, interrupt_number) == 128);
static_assert(offsetof(InterruptFrame, rip) == 144);

extern uint64_t isr_stub_table[];
extern "C" void ata_irq_handler();

extern "C" uint64_t c_interrupt_handler(InterruptFrame *frame) {
  const uint64_t vector = frame->interrupt_number;
  const uint64_t frame_addr = reinterpret_cast<uint64_t>(frame);

  if (vector == 0x30)
    return Kernel::Scheduler::schedule(frame_addr);

  if (vector == 0x31) {
    Kernel::SMP::local_apic_eoi();
    Kernel::Drivers::TSCDeadline::tick();
    return Kernel::Scheduler::schedule(frame_addr);
  }

  if (vector < 0x20 || vector > 0x2F)
    return frame_addr;

  if (vector == 0x21)
    Kernel::Drivers::Keyboard::handle_interrupt();
  else if (vector == 0x2C)
    Kernel::Drivers::Mouse::handle_interrupt();
  else if (vector == 0x2E)
    ata_irq_handler();

  if (vector == 0x20u + Kernel::Drivers::E1000::get_irq())
    Kernel::Drivers::E1000::handle_interrupt();

  if (vector >= 0x28)
    outb(0xA0, 0x20);
  outb(0x20, 0x20);
  return frame_addr;
}

static inline void set_gdt_entry(GDTEntry *entries, unsigned index,
                                 uint32_t base, uint32_t limit, uint8_t access,
                                 uint8_t gran) {
  GDTEntry &e = entries[index];
  e.limit_low = static_cast<uint16_t>(limit);
  e.base_low = static_cast<uint16_t>(base);
  e.base_middle = static_cast<uint8_t>(base >> 16);
  e.access = access;
  e.granularity = static_cast<uint8_t>(((limit >> 16) & 0x0F) | (gran & 0xF0));
  e.base_high = static_cast<uint8_t>(base >> 24);
}

static inline void set_tss_descriptor(CpuTables &tables) {
  const uint64_t base = reinterpret_cast<uint64_t>(&tables.tss);
  constexpr uint64_t limit = sizeof(Tss64) - 1;
  tables.tss_descriptor[0] =
      (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (uint64_t{0x89} << 40) |
      ((limit & 0xF0000) << 32) | ((base & 0xFF000000) << 32);
  tables.tss_descriptor[1] = base >> 32;
}

static inline void set_idt_entry(unsigned vector, uint64_t handler) {
  IDTEntry &e = idt_entries[vector];
  e.offset_low = static_cast<uint16_t>(handler);
  e.selector = KERNEL_CODE_SELECTOR;
  e.ist = 0;
  e.type_attr = IDT_INTERRUPT_GATE;
  e.offset_middle = static_cast<uint16_t>(handler >> 16);
  e.offset_high = static_cast<uint32_t>(handler >> 32);
  e.zero = 0;
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
  constexpr uint16_t selector = TSS_SELECTOR;
  __asm__ volatile("ltr %0" : : "rm"(selector) : "memory");
}

static inline void idt_flush(uint64_t idt_ptr_addr) {
  __asm__ volatile("lidt (%0)" : : "r"(idt_ptr_addr) : "memory");
}

static bool init_gdt(unsigned cpu_index) {
  if (cpu_index >= Kernel::SMP::MAX_CPUS)
    return false;

  CpuTables &tables = cpu_tables[cpu_index];
  tables.gdtr.limit = static_cast<uint16_t>(sizeof(tables.gdt) +
                                            sizeof(tables.tss_descriptor) - 1);
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
    tables.tss.ist[i] = reinterpret_cast<uint64_t>(tables.interrupt_stacks[i] +
                                                   INTERRUPT_STACK_SIZE);
  }
  tables.tss.io_map_base = sizeof(Tss64);
  set_tss_descriptor(tables);
  gdt_flush(reinterpret_cast<uint64_t>(&tables.gdtr));
  return true;
}

static void __init init_idt(void) {
  idt_ptr.limit = sizeof(idt_entries) - 1;
  idt_ptr.base = reinterpret_cast<uint64_t>(idt_entries);

  for (unsigned i = 0; i < 256; ++i) {
    set_idt_entry(i, isr_stub_table[i]);
    idt_entries[i].ist = (i == 2)              ? 4
                         : (i == 8 || i == 18) ? 3
                         : (i < 32)            ? 2
                                               : 1;
  }
  idt_flush(reinterpret_cast<uint64_t>(&idt_ptr));
}

__attribute__((always_inline)) static inline void io_wait(void) {
  outb(0x80, 0);
}

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

  pic_master_mask = 0xF9;
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

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t &eax,
                         uint32_t &ebx, uint32_t &ecx, uint32_t &edx) {
  eax = leaf;
  ecx = subleaf;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
}

static bool verify_avx2_support(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid(0, 0, eax, ebx, ecx, edx);
  if (eax < 7)
    return false;

  cpuid(1, 0, eax, ebx, ecx, edx);
  constexpr uint32_t CPUID_XSAVE = 1u << 26;
  constexpr uint32_t CPUID_OSXSAVE = 1u << 27;
  constexpr uint32_t CPUID_AVX = 1u << 28;
  constexpr uint32_t REQUIRED = CPUID_XSAVE | CPUID_OSXSAVE | CPUID_AVX;
  if ((ecx & REQUIRED) != REQUIRED)
    return false;

  uint32_t xcr0_low, xcr0_high;
  __asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
  if ((xcr0_low & 0x6u) != 0x6u)
    return false;

  cpuid(7, 0, eax, ebx, ecx, edx);
  constexpr uint32_t CPUID_AVX2 = 1u << 5;
  return (ebx & CPUID_AVX2) != 0;
}

[[noreturn]] static void halt_cpu(void) {
  __asm__ volatile("cli" ::: "memory");
  for (;;)
    __asm__ volatile("hlt" ::: "memory");
}

__init void cpu_init(void) {
  if (!init_gdt(0))
    halt_cpu();
  init_idt();
  init_pic();
  if (!verify_avx2_support())
    halt_cpu();
}

extern "C" void cpu_ap_init(uint32_t cpu_index) {
  if (!init_gdt(cpu_index) || !verify_avx2_support())
    halt_cpu();
  idt_flush(reinterpret_cast<uint64_t>(&idt_ptr));
}
