#pragma once
#include <cstdint>

namespace Kernel::Drivers::PIT {

void init(uint32_t frequency);

uint32_t get_ticks();

void tick();
} // namespace Kernel::Drivers::PIT