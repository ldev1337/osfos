
#pragma once
#include <cstddef>
#include <cstdint>
namespace Kernel::Programs::Terminal {
bool initialize();
void shutdown();
size_t columns();
size_t rows();
size_t cursor_column();
size_t cursor_row();
bool has_focus();
void clear();
void write_char(char ch, uint32_t color = 0xF4D6CD);
void write(const char *text, size_t length, uint32_t color = 0xF4D6CD);
void write_to(uint64_t thread_id, const char *text, size_t length,
              uint32_t color = 0xF4D6CD);
void flush_to(uint64_t thread_id);
void write_at(size_t column, size_t row, const char *text, uint32_t color);
void show_cursor(size_t column, size_t row, uint32_t color);
void hide_cursor(size_t column, size_t row);
int read_char();
int read_char_from(uint64_t thread_id);
bool push_input_char(int character);
bool push_input_char_to(uint64_t thread_id, int character);
size_t read_line(char *buffer, size_t capacity);
size_t read_line_from(uint64_t thread_id, char *buffer, size_t capacity);
void flush();
void present();
} // namespace Kernel::Programs::Terminal
