#include <kernel/cpu.h>
#include <kernel/drivers/pit.h>
#include <kernel/init.h>

namespace Kernel::Drivers::PIT {
namespace {
volatile uint32_t system_ticks = 0;
uint32_t tick_period_ms = 10;
} // namespace

void __init init(uint32_t frequency) {
  if (frequency == 0)
    frequency = 100;
  uint32_t divisor = 1193180 / frequency;
  tick_period_ms = (1000 + frequency / 2) / frequency;
  if (tick_period_ms == 0)
    tick_period_ms = 1;

  outb(0x43, 0x36);
  outb(0x40, static_cast<uint8_t>(divisor & 0xFF));
  outb(0x40, static_cast<uint8_t>((divisor >> 8) & 0xFF));

  uint8_t mask = inb(0x21);
  outb(0x21, mask & ~(1 << 0));
}

void tick() { ++system_ticks; }

uint32_t get_ticks() { return system_ticks * tick_period_ms; }
} // namespace Kernel::Drivers::PIT
