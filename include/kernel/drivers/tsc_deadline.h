#pragma once
#include <cstdint>

namespace Kernel::Drivers::TSCDeadline {

bool init(uint32_t frequency);
bool init_cpu();

uint32_t get_ticks();
uint32_t get_interrupt_count();

void tick();
} // namespace Kernel::Drivers::TSCDeadline