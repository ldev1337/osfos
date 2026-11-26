#pragma once


namespace Kernel::Shell {
void run();
}

extern "C" void kprintf(const char *fmt, ...);