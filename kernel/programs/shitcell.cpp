#include <algorithm>
#include <arch/x86_64/string.h>
#include <cstdint>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/appcommon.h>
#include <kernel/lib/uicommon.h>
#include <kernel/scheduler.h>

namespace Kernel::Apps {
namespace {
using namespace Kernel::AppCommon;
using Kernel::Drivers::Keyboard;
using Kernel::Drivers::Mouse;

constexpr uint32_t SHITCELL_PRIMARY = 0x5C4033, SHITCELL_DARK = 0x3D2817;
constexpr uint32_t SHITCELL_HEADER_BG = 0xE6E6E6,
                   SHITCELL_HEADER_TEXT = 0x333333;
constexpr uint32_t SHITCELL_GRID_BG = 0xFFFFFF, SHITCELL_GRID_LINE = 0xD4D4D4;
constexpr uint32_t SHITCELL_TEXT_COLOR = 0x000000,
                   SHITCELL_SEL_BORDER = 0x5C4033;
constexpr uint32_t SHITCELL_SEL_BG = 0xF5EEF8, SHITCELL_FORMULA_BG = 0xF3F3F3,
                   SHITCELL_STATUS_BG = 0xF0F0F0;

struct CellData {
  int32_t r, c;
  char text[32];
  uint32_t bg_color, text_color;
};

class SparseGrid {
  CellData *cells = nullptr;
  size_t capacity = 0, count = 0;

  void grow() {
    size_t new_cap = capacity == 0 ? 32 : capacity * 2;
    auto *new_cells = new CellData[new_cap];
    for (size_t i = 0; i < count; ++i) {
      new_cells[i] = cells[i];
    }
    delete[] cells;
    cells = new_cells;
    capacity = new_cap;
  }

public:
  SparseGrid() = default;
  ~SparseGrid() { delete[] cells; }
  void clear() { count = 0; }
  [[nodiscard]] auto get_count() const -> size_t { return count; }
  [[nodiscard]] auto get_at(size_t i) const -> const CellData & {
    return cells[i];
  }

  CellData *find(int32_t r, int32_t c) {
    for (size_t i = 0; i < count; ++i)
      if (cells[i].r == r && cells[i].c == c)
        return &cells[i];
    return nullptr;
  }

  CellData &get_or_create(int32_t r, int32_t c) {
    if (CellData *e = find(r, c))
      return *e;
    if (count >= capacity)
      grow();
    cells[count] = {.r = r,
                    .c = c,
                    .text = "",
                    .bg_color = SHITCELL_GRID_BG,
                    .text_color = SHITCELL_TEXT_COLOR};
    return cells[count++];
  }

  void set_text(int32_t r, int32_t c, const char *str) {
    CellData &cell = get_or_create(r, c);
    int i = 0;
    while (str[i] != '\0' && i < 31) {
      cell.text[i] = str[i];
      i++;
    }
    cell.text[i] = '\0';
  }

  void set_color(int32_t r, int32_t c, uint32_t bg, uint32_t fg) {
    CellData &cell = get_or_create(r, c);
    cell.bg_color = bg;
    cell.text_color = fg;
  }

  void remove(int32_t r, int32_t c) {
    for (size_t i = 0; i < count; ++i) {
      if (cells[i].r == r && cells[i].c == c) {
        for (size_t j = i; j < count - 1; ++j)
          cells[j] = cells[j + 1];
        count--;
        break;
      }
    }
  }
};

void int_to_str(int32_t val, char *buf, size_t max_len) {
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
    temp[idx++] = '0' + (val % 10);
    val /= 10;
  }
  int out_idx = 0;
  if (neg && out_idx < (int)max_len - 1)
    buf[out_idx++] = '-';
  for (int i = idx - 1; i >= 0 && out_idx < (int)max_len - 1; --i)
    buf[out_idx++] = temp[i];
  buf[out_idx] = '\0';
}

auto str_to_int(const char *str) -> int32_t {
  int32_t res = 0;
  bool neg = (str[0] == '-');
  int i = neg ? 1 : 0;
  while (str[i] >= '0' && str[i] <= '9')
    res = res * 10 + (str[i++] - '0');
  return neg ? -res : res;
}

auto str_to_double(const char *str) -> double {
  double res = 0.0;
  bool neg = false;
  int i = 0;
  if (str[0] == '-') {
    neg = true;
    i = 1;
  } else if (str[0] == '+') {
    i = 1;
  }

  while (str[i] >= '0' && str[i] <= '9') {
    res = res * 10.0 + (str[i] - '0');
    i++;
  }
  if (str[i] == '.') {
    i++;
    double frac = 1.0;
    while (str[i] >= '0' && str[i] <= '9') {
      frac /= 10.0;
      res += (str[i] - '0') * frac;
      i++;
    }
  }
  return neg ? -res : res;
}

void double_to_str(double val, char *buf, size_t max_len) {
  if (val == 0.0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  bool neg = val < 0;
  if (neg)
    val = -val;

  int int_part = static_cast<int>(val);
  double frac_part = val - int_part;

  char temp[32];
  int idx = 0;
  int iv = int_part;
  if (iv == 0) {
    temp[idx++] = '0';
  } else {
    while (iv > 0 && idx < 31) {
      temp[idx++] = '0' + (iv % 10);
      iv /= 10;
    }
  }

  int out_idx = 0;
  if (neg && out_idx < static_cast<int>(max_len) - 1)
    buf[out_idx++] = '-';
  for (int i = idx - 1; i >= 0 && out_idx < static_cast<int>(max_len) - 1;
       --i) {
    buf[out_idx++] = temp[i];
  }

  if (frac_part > 0.0001 && out_idx < static_cast<int>(max_len) - 3) {
    buf[out_idx++] = '.';
    int f1 = static_cast<int>(frac_part * 10.0) % 10;
    int f2 = static_cast<int>(frac_part * 100.0) % 10;
    buf[out_idx++] = '0' + f1;
    buf[out_idx++] = '0' + f2;
  }
  buf[out_idx] = '\0';
}

void hex_to_str(uint32_t val, char *buf) {
  const char *hex = "0123456789ABCDEF";
  for (int i = 7; i >= 0; --i)
    buf[7 - i] = hex[(val >> (i * 4)) & 0xF];
  buf[8] = '\0';
}

auto str_to_hex(const char *str) -> uint32_t {
  uint32_t res = 0;
  for (int i = 0; str[i] != '\0'; ++i) {
    char c = str[i];
    uint32_t digit = 0;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else
      break;
    res = (res << 4) | digit;
  }
  return res;
}

void get_col_name(int32_t c, char *buf, size_t max_len) {
  if (c < 0) {
    buf[0] = '-';
    buf[1] = '\0';
    return;
  }
  int32_t temp = c;
  int idx = 0;
  char temp_buf[16];
  while (temp >= 0 && idx < 15) {
    temp_buf[idx++] = static_cast<char>('A' + (temp % 26));
    temp = (temp / 26) - 1;
  }
  int rev_idx = 0;
  for (int i = idx - 1; i >= 0 && rev_idx < (int)max_len - 1; --i)
    buf[rev_idx++] = temp_buf[i];
  buf[rev_idx] = '\0';
}

void evaluate_formula(SparseGrid &grid, int current_r, int current_c,
                      const char *formula, char *result_buf, size_t max_len);

auto get_cell_or_number_val(SparseGrid &grid, const char *token, int current_r,
                            int current_c) -> double {
  if ((token[0] >= 'A' && token[0] <= 'Z') ||
      (token[0] >= 'a' && token[0] <= 'z')) {
    int col = 0;
    int i = 0;
    while ((token[i] >= 'A' && token[i] <= 'Z') ||
           (token[i] >= 'a' && token[i] <= 'z')) {
      char c = token[i];
      if (c >= 'a' && c <= 'z')
        c -= 32;
      col = col * 26 + (c - 'A' + 1);
      i++;
    }
    col--;
    int row = str_to_int(token + i) - 1;
    if (row == current_r && col == current_c)
      return 0.0;

    CellData *cell = grid.find(row, col);
    if (!cell)
      return 0.0;

    if (cell->text[0] == '=') {
      char eval_res[32];
      evaluate_formula(grid, row, col, cell->text, eval_res, sizeof(eval_res));
      return str_to_double(eval_res);
    } else {
      return str_to_double(cell->text);
    }
  } else {
    return str_to_double(token);
  }
}

void evaluate_formula(SparseGrid &grid, int current_r, int current_c,
                      const char *formula, char *result_buf, size_t max_len) {
  if (formula[0] != '=') {
    int i = 0;
    while (formula[i] != '\0' && i < (int)max_len - 1) {
      result_buf[i] = formula[i];
      i++;
    }
    result_buf[i] = '\0';
    return;
  }

  const char *expr = formula + 1;
  char tokens[16][32];
  char ops[16];
  int token_count = 0;
  int op_count = 0;
  char cur_token[32];
  int t_idx = 0;

  int i = 0;
  while (expr[i] != '\0') {
    char ch = expr[i];
    if (ch == '+' || ch == '-' || ch == '*' || ch == '/') {
      cur_token[t_idx] = '\0';
      if (t_idx > 0 && token_count < 16) {
        int k = 0;
        while (cur_token[k] != '\0' && k < 31) {
          tokens[token_count][k] = cur_token[k];
          k++;
        }
        tokens[token_count][k] = '\0';
        token_count++;
        t_idx = 0;
      }
      if (op_count < 16)
        ops[op_count++] = ch;
    } else if (ch != ' ') {
      if (t_idx < 31)
        cur_token[t_idx++] = ch;
    }
    i++;
  }
  cur_token[t_idx] = '\0';
  if (t_idx > 0 && token_count < 16) {
    int k = 0;
    while (cur_token[k] != '\0' && k < 31) {
      tokens[token_count][k] = cur_token[k];
      k++;
    }
    tokens[token_count][k] = '\0';
    token_count++;
  }

  if (token_count == 0) {
    result_buf[0] = '0';
    result_buf[1] = '\0';
    return;
  }

  double values[16];
  for (int k = 0; k < token_count; ++k) {
    values[k] = get_cell_or_number_val(grid, tokens[k], current_r, current_c);
  }

  char active_ops[16];
  for (int k = 0; k < op_count; ++k)
    active_ops[k] = ops[k];

  for (int k = 0; k < op_count; ++k) {
    if (active_ops[k] == '*' || active_ops[k] == '/') {
      double res = 0.0;
      if (active_ops[k] == '*')
        res = values[k] * values[k + 1];
      else
        res = (values[k + 1] != 0.0) ? (values[k] / values[k + 1]) : 0.0;
      values[k] = res;
      for (int j = k + 1; j < token_count - 1; ++j)
        values[j] = values[j + 1];
      for (int j = k; j < op_count - 1; ++j)
        active_ops[j] = active_ops[j + 1];
      token_count--;
      op_count--;
      k--;
    }
  }

  double final_val = values[0];
  for (int k = 0; k < op_count; ++k) {
    if (active_ops[k] == '+')
      final_val += values[k + 1];
    else if (active_ops[k] == '-')
      final_val -= values[k + 1];
  }

  double_to_str(final_val, result_buf, max_len);
}

auto save_to_vfs(SparseGrid &grid, const char *filepath) -> bool {
  if (!Kernel::FS::VFS::is_mounted())
    return false;
  void *stream = Kernel::FS::VFS::vfs_open(filepath, "w");
  if (stream == nullptr)
    return false;

  for (size_t i = 0; i < grid.get_count(); ++i) {
    const CellData &cell = grid.get_at(i);
    if (cell.text[0] == '\0' && cell.bg_color == SHITCELL_GRID_BG &&
        cell.text_color == SHITCELL_TEXT_COLOR)
      continue;
    char r[16], c[16], bg[16], fg[16];
    int_to_str(cell.r, r, sizeof(r));
    int_to_str(cell.c, c, sizeof(c));
    hex_to_str(cell.bg_color, bg);
    hex_to_str(cell.text_color, fg);
    Kernel::FS::VFS::vfs_write(r, 1, strlen(r), stream);
    Kernel::FS::VFS::vfs_write(",", 1, 1, stream);
    Kernel::FS::VFS::vfs_write(c, 1, strlen(c), stream);
    Kernel::FS::VFS::vfs_write(",", 1, 1, stream);
    Kernel::FS::VFS::vfs_write(cell.text, 1, strlen(cell.text), stream);
    Kernel::FS::VFS::vfs_write(",", 1, 1, stream);
    Kernel::FS::VFS::vfs_write(bg, 1, 8, stream);
    Kernel::FS::VFS::vfs_write(",", 1, 1, stream);
    Kernel::FS::VFS::vfs_write(fg, 1, 8, stream);
    Kernel::FS::VFS::vfs_write("\n", 1, 1, stream);
  }
  Kernel::FS::VFS::vfs_close(stream);
  return true;
}

auto load_from_vfs(SparseGrid &grid, const char *filepath) -> bool {
  if (!Kernel::FS::VFS::is_mounted())
    return false;
  void *stream = Kernel::FS::VFS::vfs_open(filepath, "r");
  if (stream == nullptr)
    return false;
  if (Kernel::FS::VFS::vfs_size(stream) == 0) {
    Kernel::FS::VFS::vfs_close(stream);
    return false;
  }

  grid.clear();
  char buf[128], line_buf[256];
  int line_len = 0;
  size_t bytes_read = 0;

  auto parse_line = [&](const char *line) {
    int r = 0, c = 0, part = 0, p_idx = 0, i = 0;
    char text[32] = {0}, p_buf[64];
    uint32_t bg = SHITCELL_GRID_BG, fg = SHITCELL_TEXT_COLOR;

    while (line[i] != '\0') {
      if (line[i] == ',' || line[i] == '\n' || line[i] == '\r') {
        p_buf[p_idx] = '\0';
        if (part == 0)
          r = str_to_int(p_buf);
        else if (part == 1)
          c = str_to_int(p_buf);
        else if (part == 2) {
          for (int k = 0; k < 31 && p_buf[k] != '\0'; ++k)
            text[k] = p_buf[k];
          text[31] = '\0';
        } else if (part == 3)
          bg = str_to_hex(p_buf);
        else if (part == 4)
          fg = str_to_hex(p_buf);
        part++;
        p_idx = 0;
      } else if (p_idx < 63)
        p_buf[p_idx++] = line[i];
      i++;
    }
    if (part >= 4) {
      p_buf[p_idx] = '\0';
      fg = str_to_hex(p_buf);
    }
    grid.set_text(r, c, text);
    grid.set_color(r, c, bg, fg);
  };

  while ((bytes_read = Kernel::FS::VFS::vfs_read(buf, 1, sizeof(buf), stream)) >
         0) {
    for (size_t i = 0; i < bytes_read; ++i) {
      char ch = buf[i];
      if (ch == '\n' || ch == '\r') {
        if (line_len > 0) {
          line_buf[line_len] = '\0';
          parse_line(line_buf);
          line_len = 0;
        }
      } else if (line_len < 255)
        line_buf[line_len++] = ch;
    }
  }
  if (line_len > 0) {
    line_buf[line_len] = '\0';
    parse_line(line_buf);
  }
  Kernel::FS::VFS::vfs_close(stream);
  return true;
}

void run_shitty_excel() {
  Kernel::Graphics::VirtualFramebuffer vfb;
  if (!vfb.open_window(1024, 720, 400, 300))
    return;
  const auto w = static_cast<int32_t>(vfb.width());
  const auto h = static_cast<int32_t>(vfb.height());

  SparseGrid grid;
  grid.set_text(0, 0, "Item");
  grid.set_color(0, 0, SHITCELL_HEADER_BG, SHITCELL_HEADER_TEXT);
  grid.set_text(0, 1, "Qty");
  grid.set_color(0, 1, SHITCELL_HEADER_BG, SHITCELL_HEADER_TEXT);
  grid.set_text(0, 2, "Price");
  grid.set_color(0, 2, SHITCELL_HEADER_BG, SHITCELL_HEADER_TEXT);
  grid.set_text(0, 3, "Total");
  grid.set_color(0, 3, SHITCELL_HEADER_BG, SHITCELL_HEADER_TEXT);
  grid.set_text(0, 4, "Formula");
  grid.set_color(0, 4, SHITCELL_HEADER_BG, SHITCELL_HEADER_TEXT);

  grid.set_text(1, 0, "Coffee");
  grid.set_text(1, 1, "10");
  grid.set_text(1, 2, "5");
  grid.set_text(1, 3, "=B2*C2");
  grid.set_text(1, 4, "=B2*5*0.5");
  grid.set_text(2, 0, "Donut");
  grid.set_text(2, 1, "24");
  grid.set_text(2, 2, "2");
  grid.set_text(2, 3, "=B3*C3");
  grid.set_text(2, 4, "=B3*2*0.5");
  grid.set_text(3, 0, "RAM");
  grid.set_text(3, 1, "2");
  grid.set_text(3, 2, "35");
  grid.set_text(3, 3, "=B4*C4");
  grid.set_text(3, 4, "=B4*35*0.5");

  int32_t sel_r = 0, sel_c = 0, view_r = 0, view_c = 0;
  bool editing = false, running = true;
  char edit_buf[32] = {0};
  int edit_len = 0;
  char current_filepath[64] = "/sheet1.csv";
  const char *status_msg = "Ready. Click cells, use Save/Open toolbar.";

  const uint32_t bg_pal[] = {SHITCELL_GRID_BG, SHITCELL_SEL_BG, 0xE3F2FD,
                             0xFFFDE7,         0xFFEBEE,        0xF5F5F5,
                             0x5C4033};
  const uint32_t text_pal[] = {
      SHITCELL_TEXT_COLOR, 0x333333, 0xD32F2F, 0x1976D2, 0x5C4033, 0xFFFFFF};
  int bg_idx = 0, text_idx = 0;
  bool prev_up = false, prev_down = false, prev_left = false,
       prev_right = false;
  bool mouse_down_prev = false;

  while (running && !Kernel::Scheduler::should_stop()) {
    int mx = Kernel::AppCommon::get_mouse_x();
    int my = Kernel::AppCommon::get_mouse_y();
    bool m_left = Kernel::AppCommon::is_left_pressed();

    int gx = 50, gy = 74, rh = 28, cw = 90;
    int v_rows = (h - gy - 22) / rh, v_cols = (w - gx - 10) / cw;

    if (m_left && !mouse_down_prev) {
      if (my >= 28 && my < 50) {

        if (mx >= 10 && mx < 70) {
          if (Kernel::UICommon::prompt_filename(
                  vfb, "Save filename:", current_filepath,
                  sizeof(current_filepath))) {
            status_msg = save_to_vfs(grid, current_filepath)
                             ? "Saved file successfully"
                             : "Save failed";
          }
        } else if (mx >= 75 && mx < 135) {
          if (Kernel::UICommon::prompt_filename(
                  vfb, "Open filename:", current_filepath,
                  sizeof(current_filepath))) {
            status_msg = load_from_vfs(grid, current_filepath)
                             ? "Loaded file successfully"
                             : "Load failed";
          }
        } else if (mx >= 140 && mx < 190) {
          running = false;
        } else if (mx >= 195 && mx < 255) {
          grid.remove(sel_r, sel_c);
          status_msg = "Cell cleared";
        } else if (mx >= 260 && mx < 320) {
          bg_idx = (bg_idx + 1) % 7;
          CellData *cell = grid.find(sel_r, sel_c);
          grid.set_color(sel_r, sel_c, bg_pal[bg_idx],
                         cell ? cell->text_color : SHITCELL_TEXT_COLOR);
          status_msg = "Background color changed";
        }
      } else if (my >= gy + rh && mx >= gx) {
        int clicked_r = view_r + (my - (gy + rh)) / rh;
        int clicked_c = view_c + (mx - gx) / cw;
        if (clicked_r >= 0 && clicked_c >= 0) {
          sel_r = clicked_r;
          sel_c = clicked_c;
          status_msg = "Cell selected via mouse";
        }
      }
    }
    mouse_down_prev = m_left;

    bool u = Keyboard::is_extended_pressed(0x48);
    bool l = Keyboard::is_extended_pressed(0x4B);
    bool r = Keyboard::is_extended_pressed(0x4D);
    bool d = Keyboard::is_extended_pressed(0x50);

    if (!editing) {
      if (u && !prev_up && sel_r > 0)
        sel_r--;
      if (d && !prev_down)
        sel_r++;
      if (l && !prev_left && sel_c > 0)
        sel_c--;
      if (r && !prev_right)
        sel_c++;
    }
    prev_up = u;
    prev_down = d;
    prev_left = l;
    prev_right = r;

    while (Keyboard::available()) {
      char c = Keyboard::getchar();
      if (editing) {
        if (c == '\n' || c == '\r') {
          edit_buf[edit_len] = '\0';
          grid.set_text(sel_r, sel_c, edit_buf);
          editing = false;
          status_msg = "Cell updated";
        } else if (c == 27) {
          editing = false;
          status_msg = "Edit canceled";
        } else if (c == '\b' && edit_len > 0) {
          edit_buf[--edit_len] = '\0';
        } else if (c >= 32 && c < 127 && edit_len < 31) {
          edit_buf[edit_len++] = c;
          edit_buf[edit_len] = '\0';
        }
      } else {
        if (c == 'q' || c == 27) {
          running = false;
          break;
        }
        char lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        if (Keyboard::is_ctrl_pressed() && lc == 's') {
          status_msg = save_to_vfs(grid, current_filepath) ? "Saved file"
                                                           : "Save failed";
        } else if (Keyboard::is_ctrl_pressed() && lc == 'l') {
          status_msg = load_from_vfs(grid, current_filepath) ? "Loaded file"
                                                             : "Load failed";
        } else if (lc == 's') {
          status_msg = save_to_vfs(grid, current_filepath) ? "Saved file"
                                                           : "Save failed";
        } else if (lc == 'l') {
          status_msg = load_from_vfs(grid, current_filepath) ? "Loaded file"
                                                             : "Load failed";
        } else if (lc == 'e' || c == '\n' || c == '\r' || c == ' ') {
          editing = true;
          CellData *cell = grid.find(sel_r, sel_c);
          int i = 0;
          if (cell != nullptr)
            while (cell->text[i] != '\0' && i < 31) {
              edit_buf[i] = cell->text[i];
              i++;
            }
          edit_buf[i] = '\0';
          edit_len = i;
          status_msg = "Editing cell...";
        } else if (lc == 'c') {
          grid.remove(sel_r, sel_c);
          status_msg = "Cell cleared";
        } else if (lc == 'f') {
          bg_idx = (bg_idx + 1) % 7;
          CellData *cell = grid.find(sel_r, sel_c);
          grid.set_color(sel_r, sel_c, bg_pal[bg_idx],
                         cell ? cell->text_color : SHITCELL_TEXT_COLOR);
          status_msg = "BG color changed";
        } else if (lc == 't') {
          text_idx = (text_idx + 1) % 6;
          CellData *cell = grid.find(sel_r, sel_c);
          grid.set_color(sel_r, sel_c, cell ? cell->bg_color : SHITCELL_GRID_BG,
                         text_pal[text_idx]);
          status_msg = "Text color changed";
        }
      }
    }
    if (!running)
      break;

    view_r = std::min(sel_r, view_r);
    if (sel_r >= view_r + v_rows)
      view_r = sel_r - v_rows + 1;
    view_c = std::min(sel_c, view_c);
    if (sel_c >= view_c + v_cols)
      view_c = sel_c - v_cols + 1;

    vfb.clear(SHITCELL_GRID_BG);
    vfb.fill_rect(0, 0, w, 28, SHITCELL_PRIMARY);
    text(vfb, 12, 10, "ShitCell - FeatureRich Spreadsheet [OSFOS]", 0xFFFFFF);
    vfb.fill_rect(0, 28, w, 22, SHITCELL_DARK);
    text(vfb, 12, 34, "Mouse Click | Save | Open | Formulas: =A1*2, =A1*A2*0.5",
         0xE0E0E0);

    Kernel::UICommon::draw_button(vfb, 10, 31, 60, 18, "Save", 0xFF45475A,
                                  0xFFFFFF, 0);
    Kernel::UICommon::draw_button(vfb, 75, 31, 60, 18, "Open", 0xFF45475A,
                                  0xFFFFFF, 0);
    Kernel::UICommon::draw_button(vfb, 140, 31, 50, 18, "Quit", 0xFFF38BA8,
                                  0xFF1E1E2E, 0);
    Kernel::UICommon::draw_button(vfb, 195, 31, 60, 18, "Clear", 0xFF45475A,
                                  0xFFFFFF, 0);
    Kernel::UICommon::draw_button(vfb, 260, 31, 60, 18, "Color", 0xFF45475A,
                                  0xFFFFFF, 0);

    int fy = 50;
    vfb.fill_rect(0, fy, w, 24, SHITCELL_FORMULA_BG);
    vfb.draw_line(0, fy + 23, w, fy + 23, SHITCELL_GRID_LINE);

    char cstr[8], rstr[16], cref[32] = "";
    get_col_name(sel_c, cstr, sizeof(cstr));
    int_to_str(sel_r + 1, rstr, sizeof(rstr));
    int ci = 0;
    for (int k = 0; cstr[k] != '\0' && ci < 30; ++k)
      cref[ci++] = cstr[k];
    for (int k = 0; rstr[k] != '\0' && ci < 30; ++k)
      cref[ci++] = rstr[k];
    cref[ci] = '\0';

    vfb.fill_rect(8, fy + 3, 46, 18, 0xFFFFFF);
    vfb.draw_rect(8, fy + 3, 46, 18, SHITCELL_GRID_LINE);
    text(vfb, 12, fy + 8, cref, SHITCELL_HEADER_TEXT);
    text(vfb, 62, fy + 8, "fx", 0x777777);

    CellData *ac = grid.find(sel_r, sel_c);
    if (editing) {
      char debuf[36];
      int idx = 0;
      while (edit_buf[idx] != '\0' && idx < 30) {
        debuf[idx] = edit_buf[idx];
        idx++;
      }
      debuf[idx] = '|';
      debuf[idx + 1] = '\0';
      text(vfb, 85, fy + 8, debuf, SHITCELL_TEXT_COLOR);
    } else {
      text(vfb, 85, fy + 8, ac ? ac->text : "", SHITCELL_TEXT_COLOR);
    }

    vfb.fill_rect(0, gy, gx, rh, SHITCELL_HEADER_BG);
    vfb.draw_rect(0, gy, gx, rh, SHITCELL_GRID_LINE);

    for (int c = 0; c < v_cols; ++c) {
      int cur_c = view_c + c, cx = gx + c * cw;
      vfb.fill_rect(cx, gy, cw, rh,
                    (cur_c == sel_c) ? 0xE8DED5 : SHITCELL_HEADER_BG);
      vfb.draw_rect(cx, gy, cw, rh, SHITCELL_GRID_LINE);
      char cn[8];
      get_col_name(cur_c, cn, sizeof(cn));
      text(vfb, cx + (cw / 2) - 8, gy + 10, cn, SHITCELL_HEADER_TEXT);
    }

    for (int r = 0; r < v_rows; ++r) {
      int cur_r = view_r + r, ry = gy + (r + 1) * rh;
      vfb.fill_rect(0, ry, gx, rh,
                    (cur_r == sel_r) ? 0xE8DED5 : SHITCELL_HEADER_BG);
      vfb.draw_rect(0, ry, gx, rh, SHITCELL_GRID_LINE);
      char rn[16];
      int_to_str(cur_r + 1, rn, sizeof(rn));
      text(vfb, 8, ry + 10, rn, SHITCELL_HEADER_TEXT);

      for (int c = 0; c < v_cols; ++c) {
        int cur_c = view_c + c, cx = gx + c * cw;
        bool sel = (cur_r == sel_r && cur_c == sel_c);
        CellData *cell = grid.find(cur_r, cur_c);
        uint32_t bg =
            sel ? SHITCELL_SEL_BG : (cell ? cell->bg_color : SHITCELL_GRID_BG);
        uint32_t fg = cell ? cell->text_color : SHITCELL_TEXT_COLOR;

        vfb.fill_rect(cx, ry, cw, rh, bg);
        vfb.draw_rect(cx, ry, cw, rh, SHITCELL_GRID_LINE);
        if (sel) {
          vfb.draw_rect(cx, ry, cw, rh, SHITCELL_SEL_BORDER);
          vfb.draw_rect(cx + 1, ry + 1, cw - 2, rh - 2, SHITCELL_SEL_BORDER);
          vfb.fill_rect(cx + cw - 4, ry + rh - 4, 4, 4, SHITCELL_SEL_BORDER);
        }

        if (sel && editing) {
          char debuf[36];
          int idx = 0;
          while (edit_buf[idx] != '\0' && idx < 30) {
            debuf[idx] = edit_buf[idx];
            idx++;
          }
          debuf[idx] = '|';
          debuf[idx + 1] = '\0';
          text(vfb, cx + 8, ry + 10, debuf, SHITCELL_TEXT_COLOR);
        } else if (cell) {
          if (cell->text[0] == '=') {
            char evaluated[32];
            evaluate_formula(grid, cur_r, cur_c, cell->text, evaluated,
                             sizeof(evaluated));
            text(vfb, cx + 8, ry + 10, evaluated, fg);
          } else {
            text(vfb, cx + 8, ry + 10, cell->text, fg);
          }
        }
      }
    }

    int ty = h - 22;
    vfb.fill_rect(0, ty, w, 22, SHITCELL_STATUS_BG);
    vfb.draw_line(0, ty, w, ty, SHITCELL_GRID_LINE);
    vfb.fill_rect(10, ty - 2, 70, 22, 0xFFFFFF);
    vfb.draw_rect(10, ty - 2, 70, 22, SHITCELL_GRID_LINE);
    vfb.fill_rect(10, ty + 18, 70, 2, SHITCELL_PRIMARY);
    text(vfb, 22, ty + 5, "Sheet1", SHITCELL_PRIMARY);
    text(vfb, 100, ty + 5, status_msg, 0x555555);

    vfb.draw_rect(mx - 3, my - 3, 7, 7, SHITCELL_PRIMARY);

    vfb.flush();
    delay(10);
  }
}

} // namespace

void shitty_excel_run() { run_shitty_excel(); }

} // namespace Kernel::Apps
