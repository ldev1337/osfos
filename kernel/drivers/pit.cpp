#include <kernel/drivers/pit.h>
#include <kernel/cpu.h>

namespace Kernel::Drivers::PIT {
namespace {
    volatile uint32_t system_ticks = 0;
}

void init(uint32_t frequency) {
    uint32_t divisor = 1193180 / frequency;

    outb(0x43, 0x36);
    outb(0x40, static_cast<uint8_t>(divisor & 0xFF));
    outb(0x40, static_cast<uint8_t>((divisor >> 8) & 0xFF));

    uint8_t mask = inb(0x21);
    outb(0x21, mask & ~(1 << 0));
}

void tick() {
    ++system_ticks;
}

uint32_t get_ticks() {
    return system_ticks * 10;
}
} 
