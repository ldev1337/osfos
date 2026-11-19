#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Programs::Terminal {
bool initialize();
size_t columns();
size_t rows();
size_t cursor_column();
size_t cursor_row();
bool has_focus();
void clear();
void write_char(char ch, uint32_t color);
void write_at(size_t column, size_t row, const char *text, uint32_t color);
void show_cursor(size_t column, size_t row, uint32_t color);
void hide_cursor(size_t column, size_t row);
void flush();
void present();
} // namespace Kernel::Programs::Terminal