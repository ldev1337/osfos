#include <kernel/drivers/tsc_deadline.h>
#include <kernel/cpu.h>
#include <kernel/debug/debug.h>
#include <kernel/smp.h>

namespace Kernel::Drivers::TSCDeadline {
namespace {
volatile uint32_t system_ticks = 0;
uint32_t tick_period_ms = 10;
uint64_t tick_period_tsc = 0;

static uint64_t calibrate_tsc_with_pit() {
  constexpr uint32_t PIT_FREQUENCY = 1193182;
  constexpr uint16_t PIT_COUNT = 59659;
  const uint8_t port61 = inb(0x61);
  outb(0x61, (port61 & ~0x02u) | 0x01u);
  outb(0x43, 0xB0);
  outb(0x42, static_cast<uint8_t>(PIT_COUNT & 0xFF));

  uint32_t lo, hi;
  __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
  const uint64_t start = (uint64_t(hi) << 32) | lo;
  outb(0x42, static_cast<uint8_t>(PIT_COUNT >> 8));

  uint32_t tries = 0;
  while (!(inb(0x61) & 0x20u) && ++tries < 1000000)
    __asm__ volatile("pause");

  __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
  const uint64_t elapsed = ((uint64_t(hi) << 32) | lo) - start;
  outb(0x61, port61);
  if (tries == 1000000 || elapsed == 0)
    return 0;
  return elapsed * PIT_FREQUENCY / PIT_COUNT;
}

static uint64_t tsc_frequency_hz() {
  uint32_t max_leaf, ebx, ecx, edx;
  __asm__ volatile("cpuid" : "=a"(max_leaf), "=b"(ebx), "=c"(ecx),
                   "=d"(edx)
                   : "a"(0), "c"(0));

  if (max_leaf >= 0x15) {
    uint32_t denominator, numerator, crystal_hz;
    __asm__ volatile("cpuid" : "=a"(denominator), "=b"(numerator),
                     "=c"(crystal_hz), "=d"(edx)
                     : "a"(0x15), "c"(0));
    if (denominator && numerator && crystal_hz)
      return static_cast<uint64_t>(crystal_hz) * numerator / denominator;
  }

  if (max_leaf >= 0x16) {
    uint32_t base_mhz;
    __asm__ volatile("cpuid" : "=a"(base_mhz), "=b"(ebx), "=c"(ecx),
                     "=d"(edx)
                     : "a"(0x16), "c"(0));
    if (base_mhz)
      return static_cast<uint64_t>(base_mhz) * 1000000;
  }
  return calibrate_tsc_with_pit();
}
} // namespace

bool init(uint32_t frequency) {
  if (frequency == 0)
    frequency = 100;
  const uint64_t tsc_hz = tsc_frequency_hz();
  if (tsc_hz == 0) {
    Kernel::Debug::log("TSC frequency unavailable in CPUID");
    return false;
  }

  tick_period_ms = (1000 + frequency / 2) / frequency;
  if (tick_period_ms == 0)
    tick_period_ms = 1;
  tick_period_tsc = (tsc_hz + frequency / 2) / frequency;
  if (tick_period_tsc == 0)
    tick_period_tsc = 1;
  if (!init_cpu()) {
    Kernel::Debug::log("local APIC TSC-deadline setup failed");
    return false;
  }
  return true;
}

bool init_cpu() {
  return tick_period_tsc != 0 &&
         Kernel::SMP::local_apic_tsc_deadline_init(tick_period_tsc);
}

void tick() {
  if (Kernel::SMP::is_bootstrap_cpu())
    ++system_ticks;
  const uint64_t deadline = Kernel::SMP::read_tsc() + tick_period_tsc;
  Kernel::SMP::local_apic_tsc_deadline_arm(deadline);
}

uint32_t get_ticks() { return system_ticks * tick_period_ms; }
uint32_t get_interrupt_count() { return system_ticks; }
} // namespace Kernel::Drivers::TSCDeadline