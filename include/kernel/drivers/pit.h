#pragma once
#include <cstdint>

namespace Kernel::Drivers::PIT {

void init(uint32_t frequency);

uint32_t get_ticks();
uint32_t get_interrupt_count();

void tick();
} // namespace Kernel::Drivers::PIT
