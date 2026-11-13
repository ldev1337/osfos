#include <arch/x86_64/string.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/lib/appcommon.h>
#include <kernel/lib/math.h>
#include <kernel/lib/uicommon.h>

#include <algorithm>

namespace Kernel::UICommon {

static int32_t font_height(float font_size_pixels) {
  return static_cast<int32_t>(font_size_pixels + 0.999f);
}

Rect FlexLayout::center(Rect bounds, int32_t width, int32_t height) {
  return {bounds.x + (bounds.width - width) / 2,
          bounds.y + (bounds.height - height) / 2, width, height};
}

Rect FlexLayout::row_item(Rect bounds, uint32_t index, uint32_t count,
                          int32_t gap) {
  if (!count || index >= count)
    return {bounds.x, bounds.y, 0, bounds.height};
  gap = std::max(gap, 0);
  const int32_t available =
      std::max(bounds.width - gap * static_cast<int32_t>(count - 1), 0);
  const int32_t base = available / static_cast<int32_t>(count);
  const uint32_t remainder = static_cast<uint32_t>(available % count);
  const int32_t offset = static_cast<int32_t>(index) * base +
                         static_cast<int32_t>(std::min(index, remainder)) +
                         static_cast<int32_t>(index) * gap;
  const int32_t width = base + (index < remainder ? 1 : 0);
  return {bounds.x + offset, bounds.y, width, bounds.height};
}

Rect FlexLayout::column_item(Rect bounds, uint32_t index, uint32_t count,
                             int32_t gap) {
  if (!count || index >= count)
    return {bounds.x, bounds.y, bounds.width, 0};
  gap = std::max(gap, 0);
  const int32_t available =
      std::max(bounds.height - gap * static_cast<int32_t>(count - 1), 0);
  const int32_t base = available / static_cast<int32_t>(count);
  const uint32_t remainder = static_cast<uint32_t>(available % count);
  const int32_t offset = static_cast<int32_t>(index) * base +
                         static_cast<int32_t>(std::min(index, remainder)) +
                         static_cast<int32_t>(index) * gap;
  const int32_t height = base + (index < remainder ? 1 : 0);
  return {bounds.x, bounds.y + offset, bounds.width, height};
}

Rect FlexLayout::grid_cell(Rect bounds, uint32_t row, uint32_t column,
                           uint32_t rows, uint32_t columns, int32_t gap_x,
                           int32_t gap_y) {
  if (!rows || !columns || row >= rows || column >= columns)
    return {bounds.x, bounds.y, 0, 0};
  const Rect row_bounds = row_item(bounds, column, columns, gap_x);
  const Rect column_bounds = column_item(bounds, row, rows, gap_y);
  return {row_bounds.x, column_bounds.y, row_bounds.width,
          column_bounds.height};
}

static inline void copy_str(char *dst, const char *src, size_t max_len) {
  size_t i = 0;
  if (src) {
    for (; i + 1 < max_len && src[i]; ++i) {
      dst[i] = src[i];
    }
  }
  dst[i] = '\0';
}

void text(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x, int32_t y,
          const char *value, uint32_t color, float font_size_pixels) {
  Kernel::AppCommon::text(vfb, x, y, value, color, font_size_pixels);
}

int text(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x, int32_t y,
         float font_size_pixels, Kernel::Graphics::TrueTypeFont &font,
         const char *value, uint32_t color) {
  return vfb.draw_text_truetype(x, y, font_size_pixels, font, value, color);
}

int text_width(const char *value, float font_size_pixels) {
  return Kernel::AppCommon::text_width(value, font_size_pixels);
}

void number(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x, int32_t y,
            int32_t value, uint32_t color) {
  Kernel::AppCommon::number(vfb, x, y, value, color);
}

int mouse_x() { return Kernel::AppCommon::get_mouse_x(); }
int mouse_y() { return Kernel::AppCommon::get_mouse_y(); }
bool primary_pressed() { return Kernel::AppCommon::is_left_pressed(); }

// --- Label Implementation ---
Label::Label(int32_t x, int32_t y, const char *initial_text, uint32_t color)
    : Widget(x, y, 0, 14), color_(color) {
  copy_str(text_, initial_text, sizeof(text_));
  w_ = Kernel::AppCommon::text_width(text_, font_size_pixels_);
}

void Label::set_font_size_pixels(float font_size_pixels) {
  Widget::set_font_size_pixels(font_size_pixels);
  w_ = Kernel::AppCommon::text_width(text_, font_size_pixels_);
  h_ = font_height(font_size_pixels_);
}

void Label::set_text(const char *new_text) {
  if (strcmp(text_, new_text) != 0) {
    copy_str(text_, new_text, sizeof(text_));
    w_ = Kernel::AppCommon::text_width(text_, font_size_pixels_);
    dirty_ = true;
  }
}

void Label::draw(Kernel::Graphics::VirtualFramebuffer &vfb) {
  Kernel::AppCommon::text(vfb, x_, y_, text_, color_, font_size_pixels_);
  dirty_ = false;
}

bool Label::handle_mouse(int, int, bool, bool) { return false; }
bool Label::handle_key(char) { return false; }

// --- Button Implementation ---
Button::Button(int32_t x, int32_t y, int32_t w, int32_t h, const char *label,
               uint32_t bg_color, uint32_t text_color, uint32_t border_color)
    : Widget(x, y, w, h), bg_color_(bg_color), text_color_(text_color),
      border_color_(border_color), hovered_(false), pressed_(false) {
  copy_str(label_, label, sizeof(label_));
}

void Widget::set_font_size_pixels(float font_size_pixels) {
  if (font_size_pixels > 0.0f && font_size_pixels_ != font_size_pixels) {
    font_size_pixels_ = font_size_pixels;
    dirty_ = true;
  }
}

void Widget::set_bounds(Rect bounds) {
  if (x_ != bounds.x || y_ != bounds.y || w_ != bounds.width ||
      h_ != bounds.height) {
    x_ = bounds.x;
    y_ = bounds.y;
    w_ = std::max(bounds.width, 0);
    h_ = std::max(bounds.height, 0);
    dirty_ = true;
  }
}

void Widget::set_visible(bool visible) {
  if (visible_ != visible) {
    visible_ = visible;
    dirty_ = true;
  }
}

void Widget::set_enabled(bool enabled) {
  if (enabled_ != enabled) {
    enabled_ = enabled;
    dirty_ = true;
  }
}

bool Button::handle_mouse(int mx, int my, bool left_pressed,
                          bool left_released) {
  bool was_hovered = hovered_;
  bool was_pressed = pressed_;
  hovered_ = (mx >= x_ && mx < x_ + w_ && my >= y_ && my < y_ + h_);
  pressed_ = hovered_ && left_pressed;

  if (was_hovered != hovered_ || was_pressed != pressed_) {
    dirty_ = true;
  }
  return hovered_ && left_released && was_pressed;
}

bool Button::handle_key(char) { return false; }

void Button::draw(Kernel::Graphics::VirtualFramebuffer &vfb) {
  uint32_t current_bg =
      pressed_ ? COLOR_OVERLAY : (hovered_ ? COLOR_SURFACE : bg_color_);
  uint32_t current_border = hovered_ ? COLOR_BLUE : border_color_;

  vfb.fill_rounded_rect(x_, y_, w_, h_, 4, current_bg);
  if (current_border != 0) {
    vfb.draw_rounded_rect(x_, y_, w_, h_, 4, current_border);
  }

  int32_t text_w = Kernel::AppCommon::text_width(label_, font_size_pixels_);
  int32_t tx = x_ + (w_ - text_w) / 2;
  int32_t ty = y_ + (h_ - font_height(font_size_pixels_)) / 2;

  Kernel::AppCommon::text(vfb, tx, ty, label_, text_color_, font_size_pixels_);
  dirty_ = false;
}

// --- Checkbox Implementation ---
Checkbox::Checkbox(int32_t x, int32_t y, const char *label,
                   bool initial_checked)
    : Widget(x, y, 14 + 8 + Kernel::AppCommon::text_width(label, 14.0f), 18),
      checked_(initial_checked), hovered_(false), prev_left_pressed_(false) {
  copy_str(label_, label, sizeof(label_));
}

void Checkbox::set_font_size_pixels(float font_size_pixels) {
  Widget::set_font_size_pixels(font_size_pixels);
  const int32_t box_size = font_height(font_size_pixels_);
  w_ = box_size + 8 + Kernel::AppCommon::text_width(label_, font_size_pixels_);
  h_ = box_size;
}

void Checkbox::set_checked(bool checked) {
  if (checked_ != checked) {
    checked_ = checked;
    dirty_ = true;
  }
}

bool Checkbox::handle_mouse(int mx, int my, bool left_pressed,
                            bool left_released) {
  (void)left_released;
  bool was_hovered = hovered_;
  hovered_ = (mx >= x_ && mx < x_ + w_ && my >= y_ && my < y_ + h_);

  if (was_hovered != hovered_) {
    dirty_ = true;
  }

  bool clicked = false;
  if (hovered_ && !left_pressed && prev_left_pressed_) {
    checked_ = !checked_;
    dirty_ = true;
    clicked = true;
  }
  prev_left_pressed_ = left_pressed;
  return clicked;
}

bool Checkbox::handle_key(char) { return false; }

void Checkbox::draw(Kernel::Graphics::VirtualFramebuffer &vfb) {
  int box_size = font_height(font_size_pixels_);
  vfb.fill_rounded_rect(x_, y_, box_size, box_size, 3, COLOR_SURFACE);
  vfb.draw_rounded_rect(x_, y_, box_size, box_size, 3,
                        hovered_ ? COLOR_BLUE : COLOR_BORDER);

  if (checked_) {
    vfb.fill_rounded_rect(x_ + 3, y_ + 3, box_size - 6, box_size - 6, 2,
                          COLOR_BLUE);
  }

  if (label_[0] != '\0') {
    Kernel::AppCommon::text(vfb, x_ + box_size + 8,
                y_ + (box_size - font_height(font_size_pixels_)) / 2,
                label_, COLOR_TEXT, font_size_pixels_);
  }
  dirty_ = false;
}

// --- Slider Implementation ---
Slider::Slider(int32_t x, int32_t y, int32_t w, int32_t h, int32_t min_val,
               int32_t max_val, int32_t initial_val)
    : Widget(x, y, w, h), min_val_(min_val), max_val_(max_val),
      val_(initial_val), hovered_(false), dragging_(false) {}

void Slider::set_value(int32_t val) {
  val = std::max(val, min_val_);
  val = std::min(val, max_val_);
  if (val_ != val) {
    val_ = val;
    dirty_ = true;
  }
}

bool Slider::handle_mouse(int mx, int my, bool left_pressed,
                          bool left_released) {
  (void)left_released;
  bool was_hovered = hovered_;
  hovered_ = (mx >= x_ && mx < x_ + w_ && my >= y_ - 4 && my < y_ + h_ + 4);

  if (was_hovered != hovered_) {
    dirty_ = true;
  }

  if (left_pressed && (hovered_ || dragging_)) {
    dragging_ = true;
    int rel_x = mx - x_;
    rel_x = std::max(rel_x, 0);
    rel_x = std::min(rel_x, w_);
    int range = max_val_ - min_val_;
    int32_t new_val = min_val_ + (rel_x * range) / (w_ > 0 ? w_ : 1);
    if (new_val != val_) {
      val_ = new_val;
      dirty_ = true;
    }
  } else if (!left_pressed) {
    dragging_ = false;
  }
  return dirty_;
}

bool Slider::handle_key(char) { return false; }

void Slider::draw(Kernel::Graphics::VirtualFramebuffer &vfb) {
  vfb.fill_rect(x_, y_, w_, h_, COLOR_BG);
  int bar_y = y_ + h_ / 2 - 2;
  vfb.fill_rounded_rect(x_, bar_y, w_, 4, 2, COLOR_SURFACE);

  int range = max_val_ - min_val_;
  int handle_x = x_ + ((val_ - min_val_) * w_) / (range > 0 ? range : 1);
  handle_x = std::max(handle_x, x_);
  handle_x = std::min(handle_x, x_ + w_);

  vfb.fill_circle(handle_x, y_ + h_ / 2, 6, hovered_ ? COLOR_BLUE : COLOR_TEXT);
  dirty_ = false;
}

// --- Status Bar Implementation ---
StatusBar::StatusBar(int32_t x, int32_t y, int32_t w, int32_t h,
                     const char *initial_text, uint32_t bg_color,
                     uint32_t text_color, uint32_t border_color)
    : Widget(x, y, w, h), bg_color_(bg_color), text_color_(text_color),
      border_color_(border_color) {
  copy_str(status_text_, initial_text, sizeof(status_text_));
}

void StatusBar::set_status(const char *new_text) {
  if (strcmp(status_text_, new_text) != 0) {
    copy_str(status_text_, new_text, sizeof(status_text_));
    dirty_ = true;
  }
}

bool StatusBar::handle_mouse(int, int, bool, bool) { return false; }
bool StatusBar::handle_key(char) { return false; }

void StatusBar::draw(Kernel::Graphics::VirtualFramebuffer &vfb) {
  vfb.fill_rect(x_, y_, w_, h_, bg_color_);
  if (border_color_ != 0) {
    vfb.draw_line(x_, y_, x_ + w_, y_, border_color_);
  }
  Kernel::AppCommon::text(vfb, x_ + 10,
                          y_ + (h_ - font_height(font_size_pixels_)) / 2,
                          status_text_, text_color_, font_size_pixels_);
  dirty_ = false;
}

// --- Text Input Implementation ---
TextInput::TextInput(int32_t x, int32_t y, int32_t w, int32_t h, char *buf,
                     size_t max_len)
    : Widget(x, y, w, h), buffer_(buf), max_len_(max_len), len_(0),
  cursor_pos_(0), first_visible_char_(0), focused_(false) {
  if (buffer_) {
    len_ = strlen(buffer_);
    cursor_pos_ = len_;
  }
}

void TextInput::set_text(const char *text) {
  if (!buffer_ || !max_len_)
    return;
  copy_str(buffer_, text, max_len_);
  len_ = strlen(buffer_);
  cursor_pos_ = len_;
  first_visible_char_ = 0;
  dirty_ = true;
}

void TextInput::set_focused(bool focused) {
  if (focused_ != focused) {
    focused_ = focused;
    dirty_ = true;
  }
}

bool TextInput::handle_mouse(int mx, int my, bool left_pressed,
                             bool left_released) {
  (void)left_released;
  if (left_pressed) {
    bool inside = (mx >= x_ && mx < x_ + w_ && my >= y_ && my < y_ + h_);
    if (inside != focused_) {
      focused_ = inside;
      dirty_ = true;
    }
    if (inside) {
      if (first_visible_char_ > len_)
        first_visible_char_ = len_;
      int rel_x = mx - (x_ + 8);
      rel_x = std::max(rel_x, 0);
      if (first_visible_char_) {
        char saved = buffer_[first_visible_char_];
        buffer_[first_visible_char_] = '\0';
        rel_x += Kernel::AppCommon::text_width(buffer_, font_size_pixels_);
        buffer_[first_visible_char_] = saved;
      }
      size_t new_cursor = 0;
      int previous_width = 0;
      for (size_t i = 0; i < len_; ++i) {
        char saved = buffer_[i + 1];
        buffer_[i + 1] = '\0';
        const int next_width =
            Kernel::AppCommon::text_width(buffer_, font_size_pixels_);
        buffer_[i + 1] = saved;
        if (rel_x < (previous_width + next_width) / 2) {
          new_cursor = i;
          break;
        }
        new_cursor = i + 1;
        previous_width = next_width;
      }
      if (new_cursor != cursor_pos_) {
        cursor_pos_ = new_cursor;
        dirty_ = true;
      }
      return true;
    }
  }
  return false;
}

bool TextInput::handle_key(char c) {
  if (!focused_)
    return false;
  if (c == '\b') {
    if (cursor_pos_ > 0 && len_ > 0) {
      for (size_t i = cursor_pos_ - 1; i < len_; ++i) {
        buffer_[i] = buffer_[i + 1];
      }
      cursor_pos_--;
      len_--;
      if (first_visible_char_ > len_)
        first_visible_char_ = len_;
      dirty_ = true;
      return true;
    }
  } else if (c >= 32 && c < 127 && len_ + 1 < max_len_) {
    for (size_t i = len_ + 1; i > cursor_pos_; --i) {
      buffer_[i] = buffer_[i - 1];
    }
    buffer_[cursor_pos_] = c;
    cursor_pos_++;
    len_++;
    if (first_visible_char_ > len_)
      first_visible_char_ = len_;
    buffer_[len_] = '\0';
    dirty_ = true;
    return true;
  }
  return false;
}

namespace {

bool read_text_line(const char *source, size_t &offset, char *line,
                    size_t capacity, int32_t max_width, float font_size) {
  if (!source || !source[offset] || capacity < 2)
    return false;
  size_t length = 0;
  while (source[offset] && source[offset] != '\n' && length + 1 < capacity) {
    line[length] = source[offset];
    line[length + 1] = '\0';
    if (length && Kernel::AppCommon::text_width(line, font_size) > max_width)
      break;
    ++length;
    ++offset;
  }
  if (source[offset] == '\n')
    ++offset;
  line[length] = '\0';
  return true;
}

size_t count_text_lines(const char *text, int32_t max_width,
                        float font_size) {
  size_t offset = 0;
  size_t lines = 0;
  char line[160];
  while (read_text_line(text, offset, line, sizeof(line), max_width,
                        font_size))
    ++lines;
  return lines;
}

} // namespace

TextArea::TextArea(int32_t x, int32_t y, int32_t w, int32_t h,
                   const char *initial_text, uint32_t background,
                   uint32_t text_color, uint32_t border_color)
    : Widget(x, y, w, h), text_(initial_text ? initial_text : ""),
      scroll_line_(0), focused_(false), background_color_(background),
      text_color_(text_color), border_color_(border_color) {}

void TextArea::set_text(const char *text_value) {
  text_value = text_value ? text_value : "";
  text_ = text_value;
  scroll_line_ = 0;
  dirty_ = true;
}

void TextArea::set_scroll_line(size_t line) {
  const int32_t page =
      std::max((h_ - 12) / (font_height(font_size_pixels_) + 3), 1);
  const size_t total = count_text_lines(text_, std::max(w_ - 16, 1),
                                        font_size_pixels_);
  const size_t maximum = total > static_cast<size_t>(page) ? total - page : 0;
  line = std::min(line, maximum);
  if (scroll_line_ != line) {
    scroll_line_ = line;
    dirty_ = true;
  }
}

void TextArea::set_focused(bool focused) {
  if (focused_ != focused) {
    focused_ = focused;
    dirty_ = true;
  }
}

void TextArea::draw(Kernel::Graphics::VirtualFramebuffer &vfb) {
  vfb.fill_rect(x_, y_, w_, h_, background_color_);
  vfb.draw_rect(x_, y_, w_, h_, focused_ ? COLOR_FOCUS : border_color_);

  const int32_t line_height = font_height(font_size_pixels_) + 3;
  const int32_t max_width = std::max(w_ - 16, 1);
  const int32_t visible_lines = std::max((h_ - 12) / line_height, 1);
  size_t offset = 0;
  char line[160];
  for (size_t skipped = 0; skipped < scroll_line_; ++skipped) {
    if (!read_text_line(text_, offset, line, sizeof(line), max_width,
                        font_size_pixels_))
      break;
  }
  int32_t y = y_ + 6;
  for (int32_t row = 0; row < visible_lines && y + line_height <= y_ + h_ - 3;
       ++row) {
    if (!read_text_line(text_, offset, line, sizeof(line), max_width,
                        font_size_pixels_))
      break;
    Kernel::AppCommon::text(vfb, x_ + 8, y, line, text_color_,
                            font_size_pixels_);
    y += line_height;
  }
  dirty_ = false;
}

bool TextArea::handle_mouse(int mx, int my, bool left_pressed, bool) {
  if (!left_pressed)
    return false;
  const bool inside = mx >= x_ && mx < x_ + w_ && my >= y_ && my < y_ + h_;
  set_focused(inside);
  return inside;
}

bool TextArea::handle_key(char c) {
  if (!focused_)
    return false;
  if (c == 'k' || c == 'K' || c == 16) {
    if (scroll_line_)
      --scroll_line_;
  } else if (c == 'j' || c == 'J' || c == 14) {
    set_scroll_line(scroll_line_ + 1);
  } else if (c == ' ') {
    const size_t page = static_cast<size_t>(std::max(
        (h_ - 12) / (font_height(font_size_pixels_) + 3), 1));
    set_scroll_line(scroll_line_ + page);
  } else if (c == 'g') {
    set_scroll_line(0);
  } else if (c == 'G') {
    const size_t page = static_cast<size_t>(std::max(
        (h_ - 12) / (font_height(font_size_pixels_) + 3), 1));
    const size_t total = count_text_lines(text_, std::max(w_ - 16, 1),
                                          font_size_pixels_);
    set_scroll_line(total > page ? total - page : 0);
  } else {
    return false;
  }
  dirty_ = true;
  return true;
}

bool WidgetGroup::add(Widget &widget) {
  if (count_ == CAPACITY)
    return false;
  for (size_t i = 0; i < count_; ++i)
    if (widgets_[i] == &widget)
      return true;
  widgets_[count_++] = &widget;
  return true;
}

void WidgetGroup::clear() {
  for (size_t i = 0; i < count_; ++i)
    widgets_[i] = nullptr;
  count_ = 0;
}

void WidgetGroup::draw(Kernel::Graphics::VirtualFramebuffer &vfb,
                       bool dirty_only) {
  for (size_t i = 0; i < count_; ++i)
    if (widgets_[i] && widgets_[i]->is_visible() &&
        (!dirty_only || widgets_[i]->is_dirty()))
      widgets_[i]->draw(vfb);
}

Widget *WidgetGroup::handle_mouse(int mx, int my, bool left_pressed,
                                  bool left_released) {
  for (size_t i = count_; i > 0; --i) {
    Widget *widget = widgets_[i - 1];
    if (widget && widget->is_visible() && widget->is_enabled() &&
        widget->handle_mouse(mx, my, left_pressed, left_released))
      return widget;
  }
  return nullptr;
}

bool WidgetGroup::handle_key(char c) {
  for (size_t i = count_; i > 0; --i)
    if (widgets_[i - 1] && widgets_[i - 1]->is_visible() &&
        widgets_[i - 1]->is_enabled() && widgets_[i - 1]->handle_key(c))
      return true;
  return false;
}

void TextInput::draw(Kernel::Graphics::VirtualFramebuffer &vfb) {
  vfb.fill_rounded_rect(x_, y_, w_, h_, 4, COLOR_SURFACE);
  vfb.draw_rounded_rect(x_, y_, w_, h_, 4,
                        focused_ ? COLOR_FOCUS : COLOR_BORDER);

  if (buffer_) {
    const size_t display_pos = cursor_pos_ > len_ ? len_ : cursor_pos_;
    const int32_t available_width = std::max(w_ - 16, 1);
    auto prefix_width = [this](size_t end) {
      const char saved = buffer_[end];
      buffer_[end] = '\0';
      const int32_t width =
          Kernel::AppCommon::text_width(buffer_, font_size_pixels_);
      buffer_[end] = saved;
      return width;
    };
    if (first_visible_char_ > display_pos)
      first_visible_char_ = display_pos;
    while (first_visible_char_ < display_pos &&
           prefix_width(display_pos) - prefix_width(first_visible_char_) >
               available_width)
      ++first_visible_char_;

    const int32_t first_width = prefix_width(first_visible_char_);
    char visible_text[128];
    size_t visible_length = 0;
    for (size_t i = first_visible_char_;
         i < len_ && visible_length + 1 < sizeof(visible_text); ++i) {
      visible_text[visible_length] = buffer_[i];
      visible_text[visible_length + 1] = '\0';
      if (visible_length &&
          Kernel::AppCommon::text_width(visible_text, font_size_pixels_) >
              available_width)
        break;
      ++visible_length;
    }
    visible_text[visible_length] = '\0';
    Kernel::AppCommon::text(vfb, x_ + 8,
                            y_ + (h_ - font_height(font_size_pixels_)) / 2,
                            visible_text, COLOR_TEXT, font_size_pixels_);

    if (focused_) {
      const int32_t cursor_x =
          x_ + 8 + prefix_width(display_pos) - first_width;
      vfb.fill_rect(cursor_x, y_ + 4, 2, h_ - 8, COLOR_BLUE);
    }
  }
  dirty_ = false;
}

// --- Modal Dialog Prompt Implementation ---
bool prompt_filename(Kernel::Graphics::VirtualFramebuffer &vfb,
                     const char *prompt, char *buf, size_t max_len) {
  TextInput input(56, 110, 308, 28, buf, max_len);
  input.set_focused(true);
  Button cancel_btn(184, 145, 80, 20, "Cancel", COLOR_SURFACE, COLOR_TEXT,
                    COLOR_BORDER);
  Button confirm_btn(274, 145, 90, 20, "Confirm", COLOR_BLUE, 0xFF1E1E2E, 0);

  // Initial draw of dialog frame
  vfb.fill_rounded_rect(40, 70, 340, 100, 8, COLOR_BG);
  vfb.draw_rounded_rect(40, 70, 340, 100, 8, COLOR_BLUE);
  Kernel::AppCommon::text(vfb, 56, 85, prompt, COLOR_TEXT);
  input.draw(vfb);
  cancel_btn.draw(vfb);
  confirm_btn.draw(vfb);
  vfb.flush();

  bool old_left = Kernel::AppCommon::is_left_pressed();

  while (true) {
    int mx = Kernel::AppCommon::get_mouse_x();
    int my = Kernel::AppCommon::get_mouse_y();
    bool left_pressed = Kernel::AppCommon::is_left_pressed();
    bool left_released = old_left && !left_pressed;
    old_left = left_pressed;

    bool needs_redraw = false;

    if (cancel_btn.handle_mouse(mx, my, left_pressed, left_released)) {
      return false;
    }
    if (confirm_btn.handle_mouse(mx, my, left_pressed, left_released)) {
      return true;
    }
    if (input.handle_mouse(mx, my, left_pressed, left_released)) {
      needs_redraw = true;
    }

    if (cancel_btn.is_dirty() || confirm_btn.is_dirty() || input.is_dirty() ||
        needs_redraw) {
      vfb.fill_rounded_rect(40, 70, 340, 100, 8, COLOR_BG);
      vfb.draw_rounded_rect(40, 70, 340, 100, 8, COLOR_BLUE);
      Kernel::AppCommon::text(vfb, 56, 85, prompt, COLOR_TEXT);
      input.draw(vfb);
      cancel_btn.draw(vfb);
      confirm_btn.draw(vfb);
      vfb.flush();
    }

    if (Kernel::Drivers::Keyboard::available()) {
      char c = Kernel::Drivers::Keyboard::getchar();
      if (c == '\n')
        return true;
      if (c == 27)
        return false;
      if (input.handle_key(c)) {
        vfb.fill_rounded_rect(40, 70, 340, 100, 8, COLOR_BG);
        vfb.draw_rounded_rect(40, 70, 340, 100, 8, COLOR_BLUE);
        Kernel::AppCommon::text(vfb, 56, 85, prompt, COLOR_TEXT);
        input.draw(vfb);
        cancel_btn.draw(vfb);
        confirm_btn.draw(vfb);
        vfb.flush();
      }
    } else {
      Kernel::Scheduler::thread_sleep_ms(10);
    }
  }
}

} // namespace Kernel::UICommon