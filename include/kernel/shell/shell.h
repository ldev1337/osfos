#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Shell {
void run();
size_t terminal_columns();
size_t terminal_rows();
void terminal_clear();
void terminal_write_at(size_t column, size_t row, const char *text,
					   uint32_t color);
void terminal_present();
}

extern "C" void kernel_printf(const char *fmt, ...);