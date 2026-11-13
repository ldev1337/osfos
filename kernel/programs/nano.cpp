#include <arch/x86_64/string.h>
#include <cstddef>
#include <cstdint>
#include <kernel/drivers/keyboard.h>
#include <kernel/fs/laziofs.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>
#include <kernel/shell/shell.h>

namespace Kernel::Apps {
namespace {
constexpr size_t DOCUMENT_CAPACITY = 16 * 1024;
constexpr size_t PATH_CAPACITY = Kernel::FS::VFS_MAX_PATH;
constexpr size_t GUTTER_WIDTH = 7;
constexpr uint32_t COLOR_NORMAL = 0xFFE6EDF0;
constexpr uint32_t COLOR_MUTED = 0xFF91A2AA;
constexpr uint32_t COLOR_ACCENT = 0xFF63C7B8;
constexpr uint32_t COLOR_WARNING = 0xFFE7B66A;

struct LineBounds {
  size_t start;
  size_t end;
};

void copy_string(char *target, size_t capacity, const char *source) {
  size_t length = 0;
  if (capacity && source) {
    while (length + 1 < capacity && source[length]) {
      target[length] = source[length];
      ++length;
    }
  }
  if (capacity)
    target[length] = '\0';
}

LineBounds line_at(const char *text, size_t length, size_t wanted) {
  size_t start = 0;
  size_t line = 0;
  while (start < length && line < wanted) {
    if (text[start++] == '\n')
      ++line;
  }
  size_t end = start;
  while (end < length && text[end] != '\n')
    ++end;
  return {start, end};
}

size_t line_number_at(const char *text, size_t cursor) {
  size_t line = 0;
  for (size_t i = 0; i < cursor; ++i)
    if (text[i] == '\n')
      ++line;
  return line;
}

void insert_char(char *text, size_t &length, size_t &cursor, char value) {
  if (length + 1 >= DOCUMENT_CAPACITY)
    return;
  for (size_t index = length + 1; index > cursor; --index)
    text[index] = text[index - 1];
  text[cursor++] = value;
  ++length;
}

void backspace(char *text, size_t &length, size_t &cursor) {
  if (!cursor)
    return;
  for (size_t index = cursor - 1; index < length; ++index)
    text[index] = text[index + 1];
  --cursor;
  --length;
}

bool load_file(const char *path, char *buffer, size_t &length) {
  if (!Kernel::FS::VFS::is_mounted())
    return false;
  void *stream = Kernel::FS::VFS::vfs_open(path, "r");
  if (!stream)
    return false;
  const size_t size = Kernel::FS::VFS::vfs_size(stream);
  if (size >= DOCUMENT_CAPACITY) {
    Kernel::FS::VFS::vfs_close(stream);
    return false;
  }
  length = Kernel::FS::VFS::vfs_read(buffer, 1, size, stream);
  buffer[length] = '\0';
  return Kernel::FS::VFS::vfs_close(stream) == 0;
}

bool save_file(const char *path, const char *buffer, size_t length) {
  if (!path || !path[0] || !Kernel::FS::VFS::is_mounted())
    return false;
  void *stream = Kernel::FS::VFS::vfs_open(path, "w");
  if (!stream)
    return false;
  const bool written =
      Kernel::FS::VFS::vfs_write(buffer, 1, length, stream) == length;
  return Kernel::FS::VFS::vfs_close(stream) == 0 && written &&
         Kernel::FS::VFS::vfs_sync();
}

bool move_vertical(const char *text, size_t length, size_t cursor, int offset,
                   size_t &destination) {
  const size_t current_line = line_number_at(text, cursor);
  const LineBounds current = line_at(text, length, current_line);
  const size_t column = cursor - current.start;
  if (offset < 0) {
    if (!current_line)
      return false;
    const LineBounds previous = line_at(text, length, current_line - 1);
    const size_t previous_length = previous.end - previous.start;
    destination = previous.start +
                  (column < previous_length ? column : previous_length);
    return true;
  }
  if (current.end >= length)
    return false;
  const LineBounds next = line_at(text, length, current_line + 1);
  const size_t next_length = next.end - next.start;
  destination = next.start + (column < next_length ? column : next_length);
  return true;
}

void write_cell(size_t column, size_t row, char character, uint32_t color) {
  char value[2] = {character, '\0'};
  Kernel::Shell::terminal_write_at(column, row, value, color);
}

void write_number(size_t column, size_t row, size_t value, uint32_t color) {
  char digits[20];
  char number[20];
  size_t length = 0;
  do {
    digits[length++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value && length < sizeof(digits));
  for (size_t i = 0; i < length; ++i)
    number[i] = digits[length - i - 1];
  number[length] = '\0';
  Kernel::Shell::terminal_write_at(column, row, number, color);
}

void draw_editor(const char *text, size_t length, size_t cursor,
                 size_t &top_line, size_t &left_column, const char *path,
                 bool modified, bool confirm_exit, const char *status) {
  const size_t columns = Kernel::Shell::terminal_columns();
  const size_t rows = Kernel::Shell::terminal_rows();
  if (columns == 0 || rows < 3)
    return;
  const size_t body_rows = rows - 2;
  const size_t text_columns = columns > GUTTER_WIDTH
                                  ? columns - GUTTER_WIDTH
                                  : 1;
  const size_t cursor_line = line_number_at(text, cursor);
  const LineBounds current = line_at(text, length, cursor_line);
  const size_t cursor_column = cursor - current.start;
  if (cursor_line < top_line)
    top_line = cursor_line;
  else if (cursor_line >= top_line + body_rows)
    top_line = cursor_line - body_rows + 1;
  if (cursor_column < left_column)
    left_column = cursor_column;
  else if (cursor_column >= left_column + text_columns)
    left_column = cursor_column - text_columns + 1;

  Kernel::Shell::terminal_clear();
  Kernel::Shell::terminal_write_at(0, 0, " nano  ", COLOR_ACCENT);
  Kernel::Shell::terminal_write_at(7, 0, path, COLOR_NORMAL);
  if (modified)
    write_cell(columns - 1, 0, '*', COLOR_WARNING);

  for (size_t row = 0; row < body_rows; ++row) {
    const size_t document_line = top_line + row;
    const LineBounds bounds = line_at(text, length, document_line);
    if (bounds.start > length)
      break;
    write_number(0, row + 1, document_line + 1, COLOR_MUTED);
    write_cell(GUTTER_WIDTH - 1, row + 1, '|', COLOR_MUTED);
    const size_t line_length = bounds.end - bounds.start;
    char visible_line[256];
    size_t visible_length = 0;
    while (visible_length < text_columns &&
           left_column + visible_length < line_length) {
      visible_line[visible_length] =
          text[bounds.start + left_column + visible_length];
      ++visible_length;
    }
    visible_line[visible_length] = '\0';
    Kernel::Shell::terminal_write_at(GUTTER_WIDTH, row + 1, visible_line,
                                     COLOR_NORMAL);
    if (document_line == cursor_line && cursor_column >= left_column &&
        cursor_column < left_column + text_columns) {
      write_cell(GUTTER_WIDTH + cursor_column - left_column, row + 1, '_',
                 COLOR_ACCENT);
    }
  }

  const size_t footer = rows - 1;
  if (confirm_exit) {
    Kernel::Shell::terminal_write_at(
        0, footer, "Unsaved changes. Ctrl+X again exits, Ctrl+S saves",
        COLOR_WARNING);
  } else if (status && status[0]) {
    Kernel::Shell::terminal_write_at(0, footer, status, COLOR_WARNING);
  } else {
    Kernel::Shell::terminal_write_at(
        0, footer, "^S Save  ^O Open  ^X Exit  Arrows/Home/End move",
        COLOR_MUTED);
    if (columns > 26) {
      const size_t row_column = columns - 22;
      Kernel::Shell::terminal_write_at(row_column, footer, "Ln", COLOR_ACCENT);
      write_number(row_column + 3, footer, cursor_line + 1, COLOR_ACCENT);
      Kernel::Shell::terminal_write_at(row_column + 12, footer, "Col",
                                       COLOR_ACCENT);
      write_number(row_column + 16, footer, cursor_column + 1, COLOR_ACCENT);
    }
  }
  Kernel::Shell::terminal_present();
}

bool prompt_path(char *path, size_t capacity) {
  const size_t columns = Kernel::Shell::terminal_columns();
  const size_t footer = Kernel::Shell::terminal_rows() - 1;
  char candidate[PATH_CAPACITY];
  copy_string(candidate, sizeof(candidate), path);
  size_t length = strlen(candidate);
  for (;;) {
    Kernel::Shell::terminal_write_at(0, footer, "Open: ", COLOR_ACCENT);
    Kernel::Shell::terminal_write_at(6, footer, candidate, COLOR_NORMAL);
    Kernel::Shell::terminal_present();
    const char key = Kernel::Drivers::Keyboard::getchar();
    if (key == 27)
      return false;
    if (key == '\n' || key == '\r') {
      copy_string(path, capacity, candidate);
      return true;
    }
    if (key == '\b') {
      if (length)
        candidate[--length] = '\0';
    } else if (key >= 32 && key < 127 && length + 1 < sizeof(candidate) &&
               length + 1 < columns - 6) {
      candidate[length++] = key;
      candidate[length] = '\0';
    }
  }
}

} // namespace

void nano_run(const char *initial_path) {
  const size_t rows = Kernel::Shell::terminal_rows();
  if (Kernel::Shell::terminal_columns() < GUTTER_WIDTH + 4 || rows < 4) {
    kernel_printf("nano: terminal is too small\n");
    return;
  }
  if (!Kernel::FS::VFS::is_mounted()) {
    kernel_printf("nano: disk is not mounted\n");
    return;
  }

  auto *document = static_cast<char *>(kmalloc(DOCUMENT_CAPACITY));
  auto *staging = static_cast<char *>(kmalloc(DOCUMENT_CAPACITY));
  if (!document || !staging) {
    if (document)
      kfree(document);
    if (staging)
      kfree(staging);
    kernel_printf("nano: not enough memory\n");
    return;
  }

  char path[PATH_CAPACITY];
  copy_string(path, sizeof(path), initial_path && initial_path[0]
                                     ? initial_path
                                     : "/untitled.txt");
  size_t length = 0;
  size_t cursor = 0;
  size_t top_line = 0;
  size_t left_column = 0;
  bool modified = false;
  bool confirm_exit = false;
  char status[64] = "New file";
  document[0] = '\0';
  if (Kernel::FS::VFS::vfs_exists(path)) {
    if (!load_file(path, document, length)) {
      strcpy(status, "Open failed or file exceeds 16 KiB");
    } else {
      status[0] = '\0';
    }
  }

  bool previous_up = false;
  bool previous_down = false;
  bool previous_left = false;
  bool previous_right = false;
  bool previous_home = false;
  bool previous_end = false;
  bool running = true;
  while (running && !Kernel::Scheduler::should_stop()) {
    draw_editor(document, length, cursor, top_line, left_column, path,
                modified, confirm_exit, status);
    status[0] = '\0';

    const bool up = Kernel::Drivers::Keyboard::is_extended_pressed(0x48);
    const bool down = Kernel::Drivers::Keyboard::is_extended_pressed(0x50);
    const bool left = Kernel::Drivers::Keyboard::is_extended_pressed(0x4B);
    const bool right = Kernel::Drivers::Keyboard::is_extended_pressed(0x4D);
    const bool home = Kernel::Drivers::Keyboard::is_extended_pressed(0x47);
    const bool end = Kernel::Drivers::Keyboard::is_extended_pressed(0x4F);
    if (up && !previous_up)
      move_vertical(document, length, cursor, -1, cursor);
    if (down && !previous_down)
      move_vertical(document, length, cursor, 1, cursor);
    if (left && !previous_left && cursor)
      --cursor;
    if (right && !previous_right && cursor < length)
      ++cursor;
    if (home && !previous_home) {
      const LineBounds line = line_at(document, length,
                                      line_number_at(document, cursor));
      cursor = line.start;
    }
    if (end && !previous_end) {
      const LineBounds line = line_at(document, length,
                                      line_number_at(document, cursor));
      cursor = line.end;
    }
    previous_up = up;
    previous_down = down;
    previous_left = left;
    previous_right = right;
    previous_home = home;
    previous_end = end;

    if (!Kernel::Drivers::Keyboard::available()) {
      Kernel::Scheduler::thread_sleep_ms(16);
      continue;
    }
    const char key = Kernel::Drivers::Keyboard::getchar();
    const char lower = key >= 'A' && key <= 'Z' ? key + ('a' - 'A') : key;
    bool changed = false;
    if (Kernel::Drivers::Keyboard::is_ctrl_pressed() && lower == 's') {
      if (save_file(path, document, length)) {
        modified = false;
        strcpy(status, "Saved");
      } else {
        strcpy(status, "Save failed");
      }
      confirm_exit = false;
    } else if (Kernel::Drivers::Keyboard::is_ctrl_pressed() && lower == 'o') {
      if (modified) {
        strcpy(status, "Save changes before opening another file");
      } else {
        char candidate[PATH_CAPACITY];
        copy_string(candidate, sizeof(candidate), path);
        if (prompt_path(candidate, sizeof(candidate))) {
        size_t loaded_length = 0;
        if (load_file(candidate, staging, loaded_length)) {
          memcpy(document, staging, loaded_length + 1);
          copy_string(path, sizeof(path), candidate);
          length = loaded_length;
          cursor = top_line = left_column = 0;
          status[0] = '\0';
        } else {
          strcpy(status, "Open failed; current document kept");
        }
        }
      }
    } else if (Kernel::Drivers::Keyboard::is_ctrl_pressed() && lower == 'x') {
      if (!modified || confirm_exit)
        running = false;
      else
        confirm_exit = true;
    } else if (key == 27) {
      if (confirm_exit)
        confirm_exit = false;
    } else if (key == '\b') {
      if (cursor) {
        backspace(document, length, cursor);
        modified = true;
        confirm_exit = false;
        changed = true;
      }
    } else if (key == '\n' || key == '\r') {
      insert_char(document, length, cursor, '\n');
      modified = true;
      confirm_exit = false;
      changed = true;
    } else if (key >= 32 && key < 127 &&
               !Kernel::Drivers::Keyboard::is_ctrl_pressed()) {
      insert_char(document, length, cursor, key);
      modified = true;
      confirm_exit = false;
      changed = true;
    }
    if (changed)
      status[0] = '\0';
  }

  Kernel::Shell::terminal_clear();
  Kernel::Shell::terminal_present();
  kfree(staging);
  kfree(document);
}

} // namespace Kernel::Apps
