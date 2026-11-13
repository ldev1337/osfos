#include <arch/x86_64/string.h>
#include <cstdint>
#include <kernel/drivers/keyboard.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/uicommon.h>
#include <kernel/lib/appcommon.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>

namespace Kernel::Apps {
namespace {
using namespace Kernel::AppCommon;
using Kernel::Drivers::Keyboard;

// Flat classic notepad palette: no gradients, no soft shading.
constexpr uint32_t BG = 0xFFFFFF;
constexpr uint32_t FG = 0x000000;
constexpr uint32_t BAR = 0xFFFFFF;
constexpr uint32_t RULE = 0x000000;
constexpr uint32_t GUTTER = 0xF7F7F7;
constexpr uint32_t MUTED = 0x555555;
constexpr uint32_t INVERT_BG = 0x000000;
constexpr uint32_t INVERT_FG = 0xFFFFFF;
constexpr uint32_t CURSOR = 0x000000;

constexpr size_t MAX_LINES = 256;
constexpr size_t MAX_LINE_LEN = 120;

constexpr int MENU_H = 20;
constexpr int STATUS_H = 18;
constexpr int GUTTER_W = 36;
constexpr int PAD_X = 6;
constexpr int PAD_Y = 2;
constexpr int ROW_H = 14;
constexpr int CHAR_W = 8;
constexpr int CURSOR_H = 14;

struct TextBuffer {
  char lines[MAX_LINES][MAX_LINE_LEN];
  size_t line_count = 1;

  TextBuffer() { clear(); }

  void clear() {
    for (size_t i = 0; i < MAX_LINES; ++i)
      lines[i][0] = '\0';
    line_count = 1;
  }

  void set_line(size_t idx, const char *str) {
    if (idx >= MAX_LINES)
      return;
    size_t i = 0;
    while (str[i] != '\0' && i < MAX_LINE_LEN - 1) {
      lines[idx][i] = str[i];
      ++i;
    }
    lines[idx][i] = '\0';
    if (idx >= line_count)
      line_count = idx + 1;
  }

  void insert_char(size_t r, size_t c, char ch) {
    if (r >= MAX_LINES)
      return;
    size_t len = 0;
    while (lines[r][len] != '\0' && len < MAX_LINE_LEN - 1)
      ++len;
    if (len >= MAX_LINE_LEN - 2)
      return;
    for (int i = (int)len; i >= (int)c; --i)
      lines[r][i + 1] = lines[r][i];
    lines[r][c] = ch;
    lines[r][len + 1] = '\0';
  }

  void delete_char(size_t r, size_t c) {
    if (r >= MAX_LINES)
      return;
    size_t len = 0;
    while (lines[r][len] != '\0')
      ++len;
    if (c >= len)
      return;
    for (size_t i = c; i < len; ++i)
      lines[r][i] = lines[r][i + 1];
  }

  void insert_line(size_t r, size_t split_c) {
    if (line_count >= MAX_LINES - 1 || r >= MAX_LINES)
      return;
    for (size_t i = line_count; i > r + 1; --i)
      for (size_t k = 0; k < MAX_LINE_LEN; ++k)
        lines[i][k] = lines[i - 1][k];

    char remainder[MAX_LINE_LEN];
    size_t ri = 0;
    for (size_t i = split_c; lines[r][i] != '\0'; ++i)
      if (ri < MAX_LINE_LEN - 1)
        remainder[ri++] = lines[r][i];
    remainder[ri] = '\0';
    lines[r][split_c] = '\0';
    for (size_t k = 0; k <= ri; ++k)
      lines[r + 1][k] = remainder[k];
    ++line_count;
  }

  void remove_line(size_t r) {
    if (line_count <= 1 || r >= line_count || r == 0)
      return;

    size_t prev_len = 0;
    while (lines[r - 1][prev_len] != '\0')
      ++prev_len;
    size_t cur_len = 0;
    while (lines[r][cur_len] != '\0')
      ++cur_len;

    if (prev_len + cur_len < MAX_LINE_LEN - 1) {
      size_t dest = prev_len;
      for (size_t i = 0; i <= cur_len; ++i)
        lines[r - 1][dest++] = lines[r][i];
    }

    for (size_t i = r; i < line_count - 1; ++i)
      for (size_t k = 0; k < MAX_LINE_LEN; ++k)
        lines[i][k] = lines[i + 1][k];
    lines[line_count - 1][0] = '\0';
    --line_count;
  }
};

void int_to_str(int32_t val, char *buf, size_t max_len) {
  if (max_len == 0)
    return;
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  bool neg = val < 0;
  if (neg)
    val = -val;
  char temp[32];
  int idx = 0;
  while (val > 0 && idx < 31) {
    temp[idx++] = static_cast<char>('0' + (val % 10));
    val /= 10;
  }
  int out = 0;
  if (neg && out < (int)max_len - 1)
    buf[out++] = '-';
  for (int i = idx - 1; i >= 0 && out < (int)max_len - 1; --i)
    buf[out++] = temp[i];
  buf[out] = '\0';
}

void append(char *dst, size_t max_len, size_t &p, const char *s) {
  while (*s && p + 1 < max_len)
    dst[p++] = *s++;
  dst[p] = '\0';
}

auto save_to_vfs(TextBuffer &buf, const char *filepath) -> bool {
  if (!Kernel::FS::VFS::is_mounted())
    return false;
  void *stream = Kernel::FS::VFS::vfs_open(filepath, "w");
  if (stream == nullptr)
    return false;

  bool saved = true;
  for (size_t i = 0; i < buf.line_count; ++i) {
    size_t line_length = strlen(buf.lines[i]);
    if (Kernel::FS::VFS::vfs_write(buf.lines[i], 1, line_length, stream) !=
            line_length ||
        Kernel::FS::VFS::vfs_write("\n", 1, 1, stream) != 1) {
      saved = false;
      break;
    }
  }
  return Kernel::FS::VFS::vfs_close(stream) == 0 && saved;
}

auto load_from_vfs(TextBuffer &buf, const char *filepath) -> bool {
  if (!Kernel::FS::VFS::is_mounted())
    return false;
  void *stream = Kernel::FS::VFS::vfs_open(filepath, "r");
  if (stream == nullptr)
    return false;

  buf.clear();
  char read_buf[128];
  size_t bytes_read = 0;
  size_t cur_line = 0;
  size_t cur_col = 0;
  bool ended_with_newline = false;
  bool previous_cr = false;

  while ((bytes_read = Kernel::FS::VFS::vfs_read(read_buf, 1, sizeof(read_buf),
                                                 stream)) > 0) {
    for (size_t i = 0; i < bytes_read; ++i) {
      char ch = read_buf[i];
      if (ch == '\n' || ch == '\r') {
        if (ch == '\n' && previous_cr) {
          previous_cr = false;
          continue;
        }
        previous_cr = ch == '\r';
        if (cur_line + 1 >= MAX_LINES)
          break;
        ++cur_line;
        cur_col = 0;
        buf.lines[cur_line][0] = '\0';
        ended_with_newline = true;
      } else {
        previous_cr = false;
        ended_with_newline = false;
        if (cur_col < MAX_LINE_LEN - 1) {
          buf.lines[cur_line][cur_col++] = ch;
          buf.lines[cur_line][cur_col] = '\0';
        }
      }
    }
    if (cur_line + 1 >= MAX_LINES)
      break;
  }
  buf.line_count = cur_line + ((ended_with_newline && cur_line > 0) ? 0 : 1);
  if (buf.line_count == 0)
    buf.line_count = 1;
  if (buf.line_count > MAX_LINES)
    buf.line_count = MAX_LINES;
  return Kernel::FS::VFS::vfs_close(stream) == 0;
}

bool hit(int mx, int my, int x, int y, int w, int h) {
  return mx >= x && mx < x + w && my >= y && my < y + h;
}

void place_cursor(TextBuffer &buf, size_t view_r, int mx, int my, int w, int h,
                  int text_x, size_t &cursor_r, size_t &cursor_c) {
  const int editor_top = MENU_H + 1;
  const int editor_bot = h - STATUS_H;
  if (my < editor_top || my >= editor_bot)
    return;

  int row = (my - editor_top - PAD_Y) / ROW_H;
  if (row < 0)
    row = 0;
  size_t target_r = view_r + static_cast<size_t>(row);
  if (target_r >= buf.line_count)
    target_r = buf.line_count - 1;

  size_t len = strlen(buf.lines[target_r]);
  const int click_x = mx - text_x;
  size_t col = 0;
  int previous_width = 0;
  char prefix[MAX_LINE_LEN]{};
  for (size_t i = 0; i < len; ++i) {
    prefix[i] = buf.lines[target_r][i];
    prefix[i + 1] = '\0';
    const int next_width = text_width(prefix);
    if (click_x < previous_width + (next_width - previous_width) / 2)
      break;
    col = i + 1;
    previous_width = next_width;
  }

  cursor_r = target_r;
  cursor_c = col;
  (void)w;
}

int cursor_pixel_offset(const char *line, size_t column) {
  char prefix[MAX_LINE_LEN]{};
  size_t length = strlen(line);
  if (column > length)
    column = length;
  for (size_t i = 0; i < column; ++i)
    prefix[i] = line[i];
  prefix[column] = '\0';
  return text_width(prefix);
}

bool prompt_path(Kernel::Graphics::VirtualFramebuffer &vfb, const char *prompt,
                 char *buf, size_t max_len) {
  using namespace Kernel::UICommon;
  const Rect dialog = FlexLayout::center(
      {0, 0, static_cast<int32_t>(vfb.width()),
       static_cast<int32_t>(vfb.height())},
      420, 116);
  Label title(dialog.x + 14, dialog.y + 10, prompt, FG);
  title.set_font_size_pixels(15.0f);
  TextInput input(dialog.x + 14, dialog.y + 38, dialog.width - 28, 24, buf,
                  max_len);
  input.set_focused(true);
  input.set_font_size_pixels(13.0f);
  Button cancel(dialog.x + dialog.width - 176, dialog.y + 72, 76, 28,
                "Cancel");
  Button confirm(dialog.x + dialog.width - 90, dialog.y + 72, 76, 28,
                 "Open", COLOR_BLUE, COLOR_BG, 0);
  cancel.set_font_size_pixels(12.0f);
  confirm.set_font_size_pixels(12.0f);
  WidgetGroup controls;
  controls.add(input);
  controls.add(cancel);
  controls.add(confirm);
  bool previous_left = Kernel::UICommon::primary_pressed();

  for (;;) {
    vfb.fill_rounded_rect(dialog.x, dialog.y, dialog.width, dialog.height, 6,
                          BG);
    vfb.draw_rounded_rect(dialog.x, dialog.y, dialog.width, dialog.height, 6,
                          RULE);
    title.draw(vfb);
    controls.draw(vfb, false);
    Kernel::UICommon::text(vfb, dialog.x + 14, dialog.y + 104,
                           "Enter confirms   Esc cancels", MUTED, 11.0f);
    vfb.flush();

    const int mx = Kernel::UICommon::mouse_x();
    const int my = Kernel::UICommon::mouse_y();
    const bool left = Kernel::UICommon::primary_pressed();
    const bool released = previous_left && !left;
    previous_left = left;
    Widget *clicked = controls.handle_mouse(mx, my, left, released);
    if (clicked == &cancel)
      return false;
    if (clicked == &confirm)
      return true;

    if (!Kernel::Drivers::Keyboard::available()) {
      Kernel::Scheduler::thread_sleep_ms(8);
      continue;
    }
    const char key = Keyboard::getchar();
    if (key == '\n' || key == '\r')
      return true;
    if (key == 27)
      return false;
    if (controls.handle_key(key))
      input.mark_dirty();
  }
}

void draw_menu_item(Kernel::Graphics::VirtualFramebuffer &vfb, int x, int y,
                    int w, int h, const char *label, bool active) {
  if (active) {
    vfb.fill_rect(x, y, w, h, INVERT_BG);
    Kernel::UICommon::text(vfb, x + 6, y + (h / 2) - 4, label, INVERT_FG);
  } else {
    Kernel::UICommon::text(vfb, x + 6, y + (h / 2) - 4, label, FG);
  }
}

void run_shitty_pad() {
  Kernel::Graphics::VirtualFramebuffer vfb;
  if (!vfb.open_window(900, 640, 480, 320))
    return;
  const auto w = static_cast<int32_t>(vfb.width());
  const auto h = static_cast<int32_t>(vfb.height());

  auto *text_buf = static_cast<TextBuffer *>(kmalloc(sizeof(TextBuffer)));
  if (!text_buf)
    return;
  text_buf->clear();
  text_buf->set_line(0, "Welcome to ShitPad.");
  text_buf->set_line(1, "Click to place the cursor. Type to edit.");
  text_buf->set_line(2, "Ctrl+S save, Ctrl+O open, Esc or Quit to exit.");

  size_t cursor_r = 0, cursor_c = 0, view_r = 0;
  char current_filepath[64] = "/note1.txt";
  const char *status_msg = "Ready";

  bool prev_up = false, prev_down = false, prev_left = false, prev_right = false;
  bool mouse_down_prev = false;

  // Flat text menu: Save | Open | New | Quit
  const int btn_y = 1;
  const int btn_h = MENU_H - 2;
  const int btn_w[] = {44, 44, 36, 44};
  const char *btn_label[] = {"Save", "Open", "New", "Quit"};
  int btn_x[4];
  btn_x[0] = 2;
  for (int i = 1; i < 4; ++i)
    btn_x[i] = btn_x[i - 1] + btn_w[i - 1] + 2;

  const int text_x = GUTTER_W + PAD_X;

  while (!Kernel::Scheduler::should_stop()) {
    const int mx = get_mouse_x();
    const int my = get_mouse_y();
    const bool m_left = is_left_pressed();

    const int editor_top = MENU_H + 1;
    const int editor_bot = h - STATUS_H;
    const int v_rows = (editor_bot - editor_top - PAD_Y) / ROW_H;
    if (v_rows < 1) {
      delay(10);
      continue;
    }

    if (m_left && !mouse_down_prev) {
      bool clicked_btn = false;
      for (int i = 0; i < 4; ++i) {
        if (!hit(mx, my, btn_x[i], btn_y, btn_w[i], btn_h))
          continue;
        clicked_btn = true;
        if (i == 0) {
          if (prompt_path(vfb, "Save as:", current_filepath,
                          sizeof(current_filepath)))
            status_msg = save_to_vfs(*text_buf, current_filepath) ? "Saved"
                                                                : "Save failed";
        } else if (i == 1) {
          if (prompt_path(vfb, "Open:", current_filepath,
                          sizeof(current_filepath))) {
            if (load_from_vfs(*text_buf, current_filepath)) {
              cursor_r = 0;
              cursor_c = 0;
              view_r = 0;
              status_msg = "Loaded";
            } else {
              status_msg = "Load failed";
            }
          }
        } else if (i == 2) {
          text_buf->clear();
          cursor_r = 0;
          cursor_c = 0;
          view_r = 0;
          status_msg = "New document";
        } else {
          goto done;
        }
        break;
      }
      if (!clicked_btn)
        place_cursor(*text_buf, view_r, mx, my, w, h, text_x, cursor_r,
                     cursor_c);
    }
    mouse_down_prev = m_left;

    bool u = Keyboard::is_extended_pressed(0x48);
    bool l = Keyboard::is_extended_pressed(0x4B);
    bool r = Keyboard::is_extended_pressed(0x4D);
    bool d = Keyboard::is_extended_pressed(0x50);

    if (u && !prev_up && cursor_r > 0) {
      --cursor_r;
      size_t len = strlen(text_buf->lines[cursor_r]);
      if (cursor_c > len)
        cursor_c = len;
    }
    if (d && !prev_down && cursor_r < text_buf->line_count - 1) {
      ++cursor_r;
      size_t len = strlen(text_buf->lines[cursor_r]);
      if (cursor_c > len)
        cursor_c = len;
    }
    if (l && !prev_left && cursor_c > 0)
      --cursor_c;
    if (r && !prev_right) {
      size_t len = strlen(text_buf->lines[cursor_r]);
      if (cursor_c < len)
        ++cursor_c;
    }
    prev_up = u;
    prev_down = d;
    prev_left = l;
    prev_right = r;

    while (Keyboard::available()) {
      char c = Keyboard::getchar();
      if (c == 27)
        goto done;
      char lc = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
      if (Keyboard::is_ctrl_pressed() && lc == 's') {
        status_msg =
            save_to_vfs(*text_buf, current_filepath) ? "Saved" : "Save failed";
      } else if (Keyboard::is_ctrl_pressed() && (lc == 'o' || lc == 'l')) {
        if (load_from_vfs(*text_buf, current_filepath)) {
          cursor_r = 0;
          cursor_c = 0;
          view_r = 0;
          status_msg = "Loaded";
        } else {
          status_msg = "Load failed";
        }
      } else if (c == '\n' || c == '\r') {
        text_buf->insert_line(cursor_r, cursor_c);
        ++cursor_r;
        cursor_c = 0;
      } else if (c == '\b') {
        if (cursor_c > 0) {
          text_buf->delete_char(cursor_r, cursor_c - 1);
          --cursor_c;
        } else if (cursor_r > 0) {
          size_t prev_len = strlen(text_buf->lines[cursor_r - 1]);
          text_buf->remove_line(cursor_r);
          --cursor_r;
          cursor_c = prev_len;
        }
      } else if (c >= 32 && c < 127) {
        text_buf->insert_char(cursor_r, cursor_c, c);
        ++cursor_c;
      }
    }

    if (Kernel::Scheduler::should_stop())
      break;

    if (cursor_r < view_r)
      view_r = cursor_r;
    if (cursor_r >= view_r + static_cast<size_t>(v_rows))
      view_r = cursor_r - static_cast<size_t>(v_rows) + 1;

    vfb.clear(BG);

    // Menu bar: flat text actions, single rule.
    vfb.fill_rect(0, 0, w, MENU_H, BAR);
    for (int i = 0; i < 4; ++i) {
      bool active =
          m_left && hit(mx, my, btn_x[i], btn_y, btn_w[i], btn_h);
      draw_menu_item(vfb, btn_x[i], btn_y, btn_w[i], btn_h, btn_label[i],
                     active);
      if (i < 3)
        Kernel::UICommon::text(vfb, btn_x[i] + btn_w[i] - 2, btn_y + (btn_h / 2) - 4, "|",
             MUTED);
    }
    vfb.draw_line(0, MENU_H, w, MENU_H, RULE);

    // Line-number gutter.
    vfb.fill_rect(0, editor_top, GUTTER_W, editor_bot - editor_top, GUTTER);
    vfb.draw_line(GUTTER_W, editor_top, GUTTER_W, editor_bot, RULE);

    for (int r = 0; r < v_rows; ++r) {
      size_t cur_r = view_r + static_cast<size_t>(r);
      if (cur_r >= text_buf->line_count)
        break;
      int ry = editor_top + PAD_Y + r * ROW_H;

      char ln[8];
      int_to_str(static_cast<int32_t>(cur_r + 1), ln, sizeof(ln));
      size_t ln_len = strlen(ln);
      int ln_x = GUTTER_W - 4 - static_cast<int>(ln_len) * CHAR_W;
      if (ln_x < 2)
        ln_x = 2;
      Kernel::UICommon::text(vfb, ln_x, ry, ln, MUTED);

      Kernel::UICommon::text(vfb, text_x, ry, text_buf->lines[cur_r], FG);
      if (cur_r == cursor_r) {
        int cx = text_x + cursor_pixel_offset(text_buf->lines[cur_r], cursor_c);
        vfb.fill_rect(cx, ry, 1, CURSOR_H, CURSOR);
      }
    }

    // Status bar: Ln / Col / status / path
    const int ty = h - STATUS_H;
    vfb.fill_rect(0, ty, w, STATUS_H, BAR);
    vfb.draw_line(0, ty, w, ty, RULE);

    char pos_str[64];
    size_t p = 0;
    char r_str[16], c_str[16], n_str[16];
    int_to_str(static_cast<int32_t>(cursor_r + 1), r_str, sizeof(r_str));
    int_to_str(static_cast<int32_t>(cursor_c + 1), c_str, sizeof(c_str));
    int_to_str(static_cast<int32_t>(text_buf->line_count), n_str, sizeof(n_str));
    append(pos_str, sizeof(pos_str), p, "Ln ");
    append(pos_str, sizeof(pos_str), p, r_str);
    append(pos_str, sizeof(pos_str), p, "  Col ");
    append(pos_str, sizeof(pos_str), p, c_str);
    append(pos_str, sizeof(pos_str), p, "  Lines ");
    append(pos_str, sizeof(pos_str), p, n_str);

    Kernel::UICommon::text(vfb, 6, ty + 5, pos_str, FG);
    Kernel::UICommon::text(vfb, 210, ty + 5, status_msg, MUTED);

    size_t path_len = strlen(current_filepath);
    int path_x = w - 6 - static_cast<int>(path_len) * CHAR_W;
    if (path_x < 320)
      path_x = 320;
    Kernel::UICommon::text(vfb, path_x, ty + 5, current_filepath, MUTED);

    vfb.flush();
    delay(10);
  }

done:
  kfree(text_buf);
  return;
}

} // namespace

void shitpad_run() { run_shitty_pad(); }

} // namespace Kernel::Apps
