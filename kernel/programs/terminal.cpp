#include <arch/x86_64/string.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/open_font.h>
#include <kernel/graphics/vfb.h>
#include <kernel/programs/terminal.h>
#include <kernel/scheduler.h>
#include <kernel/spinlock.h>

#include <cstddef>
#include <cstdint>

namespace Kernel::Programs::Terminal {
namespace {
constexpr uint32_t BG = 0x1B1111;
constexpr uint32_t FG = 0xF4D6CD;
constexpr uint32_t CELL_W = 7;
constexpr uint32_t LINE_H = 12;
constexpr size_t MAX_COLS = 256;
constexpr size_t MAX_ROWS = 64;

Graphics::VirtualFramebuffer term_vfb;
Spinlock terminal_lock;
char screen[MAX_ROWS][MAX_COLS];
uint32_t screen_fg[MAX_ROWS][MAX_COLS];
size_t cols = 0;
size_t row_count = 0;
size_t col = 0;
size_t row = 0;
uint32_t screen_w = 0;
uint32_t screen_h = 0;

Graphics::TrueTypeFont &terminal_font() {
  static Graphics::TrueTypeFont font;
  static bool initialized = false;
  if (!initialized)
    initialized =
        Graphics::OpenFont::load_preferred(font, "GeistMono.ttf", "Geist.ttf");
  return font;
}

void draw_glyph(uint32_t x, uint32_t y, char ch, uint32_t color) {
  auto &font = terminal_font();
  if (font.is_valid()) {
    char text[2] = {ch, '\0'};
    term_vfb.draw_string(static_cast<int32_t>(x), static_cast<int32_t>(y),
                         14.0f, text, color);
  } else {
    term_vfb.draw_glyph_bg(static_cast<int32_t>(x), static_cast<int32_t>(y), ch,
                           color, BG);
  }
}

void draw_cell(size_t cell_col, size_t cell_row, char ch, uint32_t color) {
  const uint32_t x = static_cast<uint32_t>(cell_col) * CELL_W;
  const uint32_t y = static_cast<uint32_t>(cell_row) * LINE_H;
  term_vfb.fill_rect(x, y, CELL_W, LINE_H, BG);
  if (ch != ' ')
    draw_glyph(x, y, ch, color);
}

void clear_row(size_t screen_row) {
  for (size_t screen_col = 0; screen_col < cols; ++screen_col) {
    screen[screen_row][screen_col] = ' ';
    screen_fg[screen_row][screen_col] = FG;
  }
}

void redraw_terminal() {
  for (size_t screen_row = 0; screen_row < row_count; ++screen_row)
    for (size_t screen_col = 0; screen_col < cols; ++screen_col)
      draw_cell(screen_col, screen_row, screen[screen_row][screen_col],
                screen_fg[screen_row][screen_col]);
}

void scroll_one_line() {
  if (row_count == 0)
    return;
  memmove(&screen[0][0], &screen[1][0],
          (row_count - 1) * MAX_COLS * sizeof(char));
  memmove(&screen_fg[0][0], &screen_fg[1][0],
          (row_count - 1) * MAX_COLS * sizeof(uint32_t));
  clear_row(row_count - 1);
  row = row_count - 1;
  redraw_terminal();
}

void next_line() {
  col = 0;
  if (row + 1 >= row_count)
    scroll_one_line();
  else
    ++row;
}

void put_char_unlocked(char ch, uint32_t color) {
  if (!cols || !row_count)
    return;
  if (ch == '\n') {
    next_line();
    return;
  }
  if (ch == '\r') {
    col = 0;
    return;
  }
  if (ch == '\t') {
    for (int i = 0; i < 4; ++i) {
      screen[row][col] = ' ';
      screen_fg[row][col] = color;
      draw_cell(col, row, ' ', color);
      if (++col >= cols)
        next_line();
    }
    return;
  }
  if (ch == '\b') {
    if (col > 0)
      --col;
    else if (row > 0) {
      --row;
      col = cols - 1;
    } else
      return;
    screen[row][col] = ' ';
    screen_fg[row][col] = FG;
    draw_cell(col, row, ' ', FG);
    return;
  }
  if (static_cast<unsigned char>(ch) < 32 ||
      static_cast<unsigned char>(ch) > 126)
    return;
  screen[row][col] = ch;
  screen_fg[row][col] = color;
  draw_cell(col, row, ch, color);
  if (++col >= cols)
    next_line();
}
} // namespace

bool initialize() {
  if (!Drivers::Framebuffer::valid())
    return false;
  SpinlockGuard guard(terminal_lock);
  screen_w = Drivers::Framebuffer::width();
  screen_h = Drivers::Framebuffer::height();
  const bool has_window =
      Graphics::WindowManager::has_window(Scheduler::current_thread_id());
  if (has_window) {
    if (!term_vfb.open_window(screen_w > 48 ? screen_w - 48 : screen_w,
                              screen_h > 100 ? screen_h - 100 : screen_h,
                              CELL_W, LINE_H))
      return false;
    screen_w = term_vfb.width();
    screen_h = term_vfb.height();
  } else if (!term_vfb.init(0, 0, screen_w, screen_h)) {
    return false;
  }
  if (screen_w == 0 || screen_h < LINE_H)
    return false;

  cols = screen_w / CELL_W;
  row_count = screen_h / LINE_H;
  if (cols > MAX_COLS)
    cols = MAX_COLS;
  if (row_count > MAX_ROWS)
    row_count = MAX_ROWS;
  if (cols == 0 || row_count == 0)
    return false;

  memset(screen, ' ', sizeof(screen));
  for (size_t screen_row = 0; screen_row < row_count; ++screen_row)
    clear_row(screen_row);
  row = col = 0;
  term_vfb.clear(BG);
  redraw_terminal();
  term_vfb.flush();
  return true;
}

size_t columns() { return cols; }
size_t rows() { return row_count; }
size_t cursor_column() { return col; }
size_t cursor_row() { return row; }

bool has_focus() { return term_vfb.has_focus(); }

void clear() {
  SpinlockGuard guard(terminal_lock);
  for (size_t screen_row = 0; screen_row < row_count; ++screen_row)
    clear_row(screen_row);
  row = col = 0;
  term_vfb.clear(BG);
}

void write_char(char ch, uint32_t color) {
  SpinlockGuard guard(terminal_lock);
  put_char_unlocked(ch, color);
}

void write_at(size_t column, size_t screen_row, const char *text,
              uint32_t color) {
  if (!text || screen_row >= row_count || column >= cols)
    return;
  SpinlockGuard guard(terminal_lock);
  while (*text && column < cols) {
    const unsigned char value = static_cast<unsigned char>(*text++);
    screen[screen_row][column] =
        value >= 32 && value <= 126 ? static_cast<char>(value) : ' ';
    screen_fg[screen_row][column] = color;
    ++column;
  }
}

void show_cursor(size_t column, size_t screen_row, uint32_t color) {
  SpinlockGuard guard(terminal_lock);
  if (column < cols && screen_row < row_count) {
    const uint32_t x = static_cast<uint32_t>(column) * CELL_W;
    const uint32_t y = static_cast<uint32_t>(screen_row) * LINE_H + LINE_H - 1;
    if (x + CELL_W <= screen_w)
      term_vfb.fill_rect(x, y, CELL_W, 1, color);
    term_vfb.flush();
  }
}

void hide_cursor(size_t column, size_t screen_row) {
  SpinlockGuard guard(terminal_lock);
  if (column < cols && screen_row < row_count) {
    draw_cell(column, screen_row, screen[screen_row][column],
              screen_fg[screen_row][column]);
    term_vfb.flush();
  }
}

void flush() { term_vfb.flush(); }

void present() {
  SpinlockGuard guard(terminal_lock);
  redraw_terminal();
  term_vfb.flush();
}
} // namespace Kernel::Programs::Terminal
