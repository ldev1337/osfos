#include <cstddef>
#include <cstdint>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/open_font.h>
#include <kernel/programs/terminal.h>
#include <kernel/scheduler.h>
#include <kernel/spinlock.h>
#include <oslib/win.h>
#include <string.h>

namespace Kernel::Programs::Terminal {
namespace {
constexpr uint32_t BG = 0x1B1111;
constexpr uint32_t FG = 0xF4D6CD;
constexpr uint32_t CURSOR = 0xFAB489;
constexpr uint32_t CELL_W = 7;
constexpr uint32_t LINE_H = 12;
constexpr size_t MAX_COLS = 256;
constexpr size_t MAX_ROWS = 64;
constexpr size_t MAX_TERMINALS = 32;
constexpr size_t SCREEN_CELLS = MAX_COLS * MAX_ROWS;
enum class ContextState : uint8_t { FREE, INITIALIZING, READY, CLOSING };
struct TerminalContext {
  ContextState state = ContextState::FREE;
  uint64_t thread_id = 0;
  oslib::Win win;
  Spinlock lock;
  char *screen = nullptr;
  uint32_t *screen_fg = nullptr;
  char input_buffer[256]{};
  unsigned char input_pushback[8]{};
  size_t input_position = 0, input_length = 0, input_pushback_count = 0;
  size_t cols = 0, row_count = 0, col = 0, row = 0;
  uint32_t screen_w = 0, screen_h = 0;
};
TerminalContext contexts[MAX_TERMINALS];
Spinlock registry_lock;
uint64_t primary_thread_id = 0;

Graphics::TrueTypeFont &terminal_font() {
  static Graphics::TrueTypeFont font;
  static bool attempted = false;
  static Spinlock font_lock;
  if (!__atomic_load_n(&attempted, __ATOMIC_ACQUIRE)) {
    SpinlockGuard guard(font_lock);
    if (!attempted) {
      (void)Graphics::OpenFont::load_preferred(font, "GeistMono.ttf",
                                               "Geist.ttf");
      __atomic_store_n(&attempted, true, __ATOMIC_RELEASE);
    }
  }
  return font;
}

TerminalContext *find_ready_locked(uint64_t thread_id) {
  if (!thread_id)
    return nullptr;
  for (size_t i = 0; i < MAX_TERMINALS; ++i)
    if (contexts[i].state == ContextState::READY &&
        contexts[i].thread_id == thread_id)
      return &contexts[i];
  return nullptr;
}

TerminalContext *lock_context(bool allow_primary) {
  const uint64_t tid = Scheduler::current_thread_id();
  registry_lock.lock();
  TerminalContext *context = find_ready_locked(tid);
  if (!context && allow_primary)
    context = find_ready_locked(primary_thread_id);
  if (context)
    context->lock.lock();
  registry_lock.unlock();
  return context;
}

TerminalContext *lock_context_for(uint64_t thread_id) {
  registry_lock.lock();
  TerminalContext *context = find_ready_locked(thread_id);
  if (context)
    context->lock.lock();
  registry_lock.unlock();
  return context;
}

void unlock_context(TerminalContext *context) {
  if (context)
    context->lock.unlock();
}

void draw_glyph(TerminalContext &context, uint32_t x, uint32_t y, char ch,
                uint32_t color) {
  auto &font = terminal_font();
  if (font.is_valid()) {
    char text[2] = {ch, '\0'};
    context.win.draw_string(static_cast<int32_t>(x), static_cast<int32_t>(y),
                            14.0f, text, color);
  } else {
    context.win.draw_glyph_bg(static_cast<int32_t>(x), static_cast<int32_t>(y),
                              ch, color, BG);
  }
}

void draw_cell(TerminalContext &context, size_t cell_col, size_t cell_row,
               char ch, uint32_t color) {
  const uint32_t x = static_cast<uint32_t>(cell_col) * CELL_W;
  const uint32_t y = static_cast<uint32_t>(cell_row) * LINE_H;
  context.win.fill_rect(x, y, CELL_W, LINE_H, BG);
  if (ch != ' ')
    draw_glyph(context, x, y, ch, color);
}

void clear_row(TerminalContext &context, size_t screen_row) {
  for (size_t screen_col = 0; screen_col < context.cols; ++screen_col) {
    context.screen[screen_row * MAX_COLS + screen_col] = ' ';
    context.screen_fg[screen_row * MAX_COLS + screen_col] = FG;
  }
}

void clear_unlocked(TerminalContext &context) {
  for (size_t screen_row = 0; screen_row < context.row_count; ++screen_row)
    clear_row(context, screen_row);
  context.row = context.col = 0;
  context.win.clear(BG);
}

void redraw_terminal(TerminalContext &context) {
  for (size_t screen_row = 0; screen_row < context.row_count; ++screen_row)
    for (size_t screen_col = 0; screen_col < context.cols; ++screen_col) {
      const size_t index = screen_row * MAX_COLS + screen_col;
      draw_cell(context, screen_col, screen_row, context.screen[index],
                context.screen_fg[index]);
    }
}

void scroll_one_line(TerminalContext &context) {
  if (!context.row_count)
    return;
  memmove(context.screen, context.screen + MAX_COLS,
          (context.row_count - 1) * MAX_COLS);
  memmove(context.screen_fg, context.screen_fg + MAX_COLS,
          (context.row_count - 1) * MAX_COLS * sizeof(uint32_t));
  clear_row(context, context.row_count - 1);
  context.row = context.row_count - 1;
  redraw_terminal(context);
}

void next_line(TerminalContext &context) {
  context.col = 0;
  if (context.row + 1 >= context.row_count)
    scroll_one_line(context);
  else
    ++context.row;
}

void put_char_unlocked(TerminalContext &context, char ch, uint32_t color) {
  if (!context.cols || !context.row_count)
    return;
  if (ch == '\f') {
    clear_unlocked(context);
    return;
  }
  if (ch == '\n') {
    next_line(context);
    return;
  }
  if (ch == '\r') {
    context.col = 0;
    return;
  }
  if (ch == '\t') {
    for (int i = 0; i < 4; ++i) {
      const size_t index = context.row * MAX_COLS + context.col;
      context.screen[index] = ' ';
      context.screen_fg[index] = color;
      draw_cell(context, context.col, context.row, ' ', color);
      if (++context.col >= context.cols)
        next_line(context);
    }
    return;
  }
  if (ch == '\b') {
    if (context.col > 0)
      --context.col;
    else if (context.row > 0) {
      --context.row;
      context.col = context.cols - 1;
    } else
      return;
    const size_t index = context.row * MAX_COLS + context.col;
    context.screen[index] = ' ';
    context.screen_fg[index] = FG;
    draw_cell(context, context.col, context.row, ' ', FG);
    return;
  }
  if (static_cast<unsigned char>(ch) < 32 ||
      static_cast<unsigned char>(ch) > 126)
    return;
  const size_t index = context.row * MAX_COLS + context.col;
  context.screen[index] = ch;
  context.screen_fg[index] = color;
  draw_cell(context, context.col, context.row, ch, color);
  if (++context.col >= context.cols)
    next_line(context);
}

bool focused(uint64_t tid) {
  return !Graphics::WindowManager::has_window(tid) ||
         Graphics::WindowManager::has_keyboard_focus(tid);
}
} // namespace

bool initialize() {
  const uint64_t tid = Scheduler::current_thread_id();
  if (!tid || !Drivers::Framebuffer::valid())
    return false;
  registry_lock.lock();
  if (find_ready_locked(tid)) {
    registry_lock.unlock();
    return true;
  }
  TerminalContext *context = nullptr;
  for (size_t i = 0; i < MAX_TERMINALS; ++i) {
    if (contexts[i].state == ContextState::FREE) {
      context = &contexts[i];
      context->state = ContextState::INITIALIZING;
      context->thread_id = tid;
      break;
    }
  }
  registry_lock.unlock();
  if (!context)
    return false;

  context->lock.lock();
  context->screen = static_cast<char *>(malloc(SCREEN_CELLS));
  context->screen_fg =
      static_cast<uint32_t *>(malloc(SCREEN_CELLS * sizeof(uint32_t)));
  bool ok = context->screen && context->screen_fg;
  if (ok) {
    context->screen_w = Drivers::Framebuffer::width();
    context->screen_h = Drivers::Framebuffer::height();
    if (Graphics::WindowManager::has_window(tid)) {
      ok = context->win.open(
          context->screen_w > 48 ? context->screen_w - 48 : context->screen_w,
          context->screen_h > 100 ? context->screen_h - 100 : context->screen_h,
          CELL_W, LINE_H);
      if (ok) {
        context->screen_w = context->win.width();
        context->screen_h = context->win.height();
      }
    } else {
      ok = context->win.open_screen(0, 0, context->screen_w, context->screen_h);
    }
  }
  if (ok && context->screen_w && context->screen_h >= LINE_H) {
    context->cols = context->screen_w / CELL_W;
    context->row_count = context->screen_h / LINE_H;
    if (context->cols > MAX_COLS)
      context->cols = MAX_COLS;
    if (context->row_count > MAX_ROWS)
      context->row_count = MAX_ROWS;
    ok = context->cols && context->row_count;
  } else {
    ok = false;
  }
  if (ok) {
    memset(context->screen, ' ', SCREEN_CELLS);
    for (size_t r = 0; r < context->row_count; ++r)
      clear_row(*context, r);
    context->row = context->col = 0;
    context->win.clear(BG);
    redraw_terminal(*context);
    context->win.flush();
  }
  if (!ok) {
    context->win.close();
    free(context->screen);
    free(context->screen_fg);
    context->screen = nullptr;
    context->screen_fg = nullptr;
  }
  context->lock.unlock();

  registry_lock.lock();
  if (ok) {
    context->state = ContextState::READY;
    if (!primary_thread_id)
      primary_thread_id = tid;
  } else {
    context->state = ContextState::FREE;
    context->thread_id = 0;
    context->cols = context->row_count = context->col = context->row = 0;
    context->screen_w = context->screen_h = 0;
  }
  registry_lock.unlock();
  return ok;
}

void shutdown() {
  const uint64_t tid = Scheduler::current_thread_id();
  registry_lock.lock();
  TerminalContext *context = nullptr;
  for (size_t i = 0; i < MAX_TERMINALS; ++i)
    if (contexts[i].state == ContextState::READY &&
        contexts[i].thread_id == tid) {
      context = &contexts[i];
      context->state = ContextState::CLOSING;
      context->lock.lock();
      break;
    }
  registry_lock.unlock();
  if (!context)
    return;
  context->win.close();
  free(context->screen);
  free(context->screen_fg);
  context->screen = nullptr;
  context->screen_fg = nullptr;
  context->cols = context->row_count = context->col = context->row = 0;
  context->screen_w = context->screen_h = 0;
  context->input_position = context->input_length =
      context->input_pushback_count = 0;
  context->lock.unlock();

  registry_lock.lock();
  if (primary_thread_id == tid) {
    primary_thread_id = 0;
    for (size_t i = 0; i < MAX_TERMINALS; ++i)
      if (contexts[i].state == ContextState::READY) {
        primary_thread_id = contexts[i].thread_id;
        break;
      }
  }
  context->thread_id = 0;
  context->state = ContextState::FREE;
  registry_lock.unlock();
}

size_t columns() {
  TerminalContext *context = lock_context(false);
  const size_t value = context ? context->cols : 0;
  unlock_context(context);
  return value;
}
size_t rows() {
  TerminalContext *context = lock_context(false);
  const size_t value = context ? context->row_count : 0;
  unlock_context(context);
  return value;
}
size_t cursor_column() {
  TerminalContext *context = lock_context(false);
  const size_t value = context ? context->col : 0;
  unlock_context(context);
  return value;
}
size_t cursor_row() {
  TerminalContext *context = lock_context(false);
  const size_t value = context ? context->row : 0;
  unlock_context(context);
  return value;
}
bool has_focus() { return focused(Scheduler::current_thread_id()); }

void clear() {
  TerminalContext *context = lock_context(false);
  if (context) {
    clear_unlocked(*context);
    context->win.flush();
  }
  unlock_context(context);
}

void write_char(char ch, uint32_t color) {
  TerminalContext *context = lock_context(true);
  if (context)
    put_char_unlocked(*context, ch, color);
  unlock_context(context);
}

void write(const char *text, size_t length, uint32_t color) {
  if (!text || !length)
    return;
  TerminalContext *context = lock_context(true);
  if (context)
    for (size_t i = 0; i < length; ++i)
      put_char_unlocked(*context, text[i], color);
  unlock_context(context);
}

void write_to(uint64_t thread_id, const char *text, size_t length,
              uint32_t color) {
  if (!thread_id || !text || !length)
    return;
  TerminalContext *context = lock_context_for(thread_id);
  if (context)
    for (size_t i = 0; i < length; ++i)
      put_char_unlocked(*context, text[i], color);
  unlock_context(context);
}

void flush_to(uint64_t thread_id) {
  TerminalContext *context = lock_context_for(thread_id);
  if (context)
    context->win.flush();
  unlock_context(context);
}

void write_at(size_t column, size_t screen_row, const char *text,
              uint32_t color) {
  if (!text)
    return;
  TerminalContext *context = lock_context(false);
  if (!context)
    return;
  if (screen_row < context->row_count && column < context->cols) {
    while (*text && column < context->cols) {
      const unsigned char value = static_cast<unsigned char>(*text++);
      const size_t index = screen_row * MAX_COLS + column;
      context->screen[index] =
          value >= 32 && value <= 126 ? static_cast<char>(value) : ' ';
      context->screen_fg[index] = color;
      ++column;
    }
  }
  unlock_context(context);
}

void show_cursor(size_t column, size_t screen_row, uint32_t color) {
  TerminalContext *context = lock_context(false);
  if (context && column < context->cols && screen_row < context->row_count) {
    const uint32_t x = static_cast<uint32_t>(column) * CELL_W;
    const uint32_t y = static_cast<uint32_t>(screen_row) * LINE_H + LINE_H - 1;
    if (x + CELL_W <= context->screen_w)
      context->win.fill_rect(x, y, CELL_W, 1, color);
    context->win.flush();
  }
  unlock_context(context);
}

void hide_cursor(size_t column, size_t screen_row) {
  TerminalContext *context = lock_context(false);
  if (context && column < context->cols && screen_row < context->row_count) {
    const size_t index = screen_row * MAX_COLS + column;
    draw_cell(*context, column, screen_row, context->screen[index],
              context->screen_fg[index]);
    context->win.flush();
  }
  unlock_context(context);
}

namespace {
int read_key(uint64_t input_owner_thread_id) {
  if (!input_owner_thread_id)
    return -1;
  while (!Scheduler::should_stop()) {
    char value = 0;
    if (Drivers::Keyboard::try_getchar_for(input_owner_thread_id, &value))
      return static_cast<unsigned char>(value);
    Scheduler::thread_sleep_ms(2);
  }
  return -1;
}
} // namespace

int read_char() { return read_char_from(Scheduler::current_thread_id()); }

int read_char_from(uint64_t thread_id) {
  TerminalContext *context = lock_context_for(thread_id);
  if (!context)
    return read_key(thread_id);
  if (context->input_pushback_count) {
    const int value = context->input_pushback[--context->input_pushback_count];
    unlock_context(context);
    return value;
  }
  if (context->input_position < context->input_length) {
    const int value = static_cast<unsigned char>(
        context->input_buffer[context->input_position++]);
    if (context->input_position >= context->input_length)
      context->input_position = context->input_length = 0;
    unlock_context(context);
    return value;
  }
  unlock_context(context);

  char line[256];
  const size_t length = read_line_from(thread_id, line, sizeof(line));
  if (!length)
    return -1;
  context = lock_context_for(thread_id);
  if (!context)
    return -1;
  memcpy(context->input_buffer, line, length);
  context->input_position = 1;
  context->input_length = length;
  const int value = static_cast<unsigned char>(context->input_buffer[0]);
  if (length == 1)
    context->input_position = context->input_length = 0;
  unlock_context(context);
  return value;
}

bool push_input_char(int character) {
  return push_input_char_to(Scheduler::current_thread_id(), character);
}

bool push_input_char_to(uint64_t thread_id, int character) {
  if (character < 0 || character > 255)
    return false;
  TerminalContext *context = lock_context_for(thread_id);
  if (!context)
    return false;
  const bool ok =
      context->input_pushback_count < sizeof(context->input_pushback);
  if (ok)
    context->input_pushback[context->input_pushback_count++] =
        static_cast<unsigned char>(character);
  unlock_context(context);
  return ok;
}

size_t read_line(char *buffer, size_t capacity) {
  return read_line_from(Scheduler::current_thread_id(), buffer, capacity);
}

size_t read_line_from(uint64_t thread_id, char *buffer, size_t capacity) {
  if (!buffer || !capacity)
    return 0;
  size_t length = 0;
  while (length + 1 < capacity) {
    while (!focused(thread_id)) {
      if (Scheduler::should_stop()) {
        buffer[length] = '\0';
        return length;
      }
      Scheduler::thread_sleep_ms(4);
    }

    TerminalContext *context = lock_context_for(thread_id);
    if (!context) {
      buffer[length] = '\0';
      return length;
    }
    const size_t old_col = context->col;
    const size_t old_row = context->row;
    if (old_col < context->cols && old_row < context->row_count) {
      const uint32_t x = static_cast<uint32_t>(old_col) * CELL_W;
      const uint32_t y = static_cast<uint32_t>(old_row) * LINE_H + LINE_H - 1;
      if (x + CELL_W <= context->screen_w)
        context->win.fill_rect(x, y, CELL_W, 1, CURSOR);
      context->win.flush();
    }
    unlock_context(context);

    const int key = read_key(thread_id);

    context = lock_context_for(thread_id);
    if (!context) {
      buffer[length] = '\0';
      return length;
    }
    if (old_col < context->cols && old_row < context->row_count) {
      const size_t index = old_row * MAX_COLS + old_col;
      draw_cell(*context, old_col, old_row, context->screen[index],
                context->screen_fg[index]);
      context->win.flush();
    }
    unlock_context(context);

    if (key < 0) {
      buffer[length] = '\0';
      return length;
    }
    const char ch = static_cast<char>(key);
    if (ch == '\r' || ch == '\n') {
      buffer[length++] = '\n';
      write_to(thread_id, "\n", 1, FG);
      break;
    }
    if (ch == '\b' || static_cast<unsigned char>(ch) == 127) {
      if (length) {
        --length;
        write_to(thread_id, "\b", 1, FG);
      }
      continue;
    }
    if (static_cast<unsigned char>(ch) >= 32 &&
        static_cast<unsigned char>(ch) <= 126) {
      buffer[length++] = ch;
      write_to(thread_id, &ch, 1, FG);
    }
  }
  buffer[length] = '\0';
  flush_to(thread_id);
  return length;
}

void flush() {
  TerminalContext *context = lock_context(true);
  if (context)
    context->win.flush();
  unlock_context(context);
}

void present() {
  TerminalContext *context = lock_context(false);
  if (context) {
    redraw_terminal(*context);
    context->win.flush();
  }
  unlock_context(context);
}
} // namespace Kernel::Programs::Terminal
