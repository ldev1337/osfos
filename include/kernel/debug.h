#pragma once

#include <cstdint>

namespace Kernel::Debug {

void init();
void log(const char *message);
void log_hex(const char *label, uint64_t value);
void log_dec(const char *label, uint64_t value);
void show_welcome();
void wait_seconds(uint32_t seconds);

} // namespace Kernel::Debug
