#pragma once

namespace Kernel::Shell {
void run();
int process_entry(int argc, char **argv, void *context);
} // namespace Kernel::Shell

extern "C" void kprintf(const char *fmt, ...);
