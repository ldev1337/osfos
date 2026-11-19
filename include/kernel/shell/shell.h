#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Shell {
void run();
}

extern "C" void kprintf(const char *fmt, ...);