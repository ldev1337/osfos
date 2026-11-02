#pragma once

namespace Kernel::Shell {
void run();
}

extern "C" void kernel_printf(const char *fmt, ...);