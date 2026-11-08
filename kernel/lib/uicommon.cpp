#include <arch/x86_64/string.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/lib/appcommon.h>
#include <kernel/lib/math.h>
#include <kernel/lib/uicommon.h>

#include <algorithm>

namespace Kernel::UICommon {

static inline void copy_str(char *dst, const char *src, size_t max_len) {
  size_t i = 0;
  if (src) {
    for (; i + 1 < max_len && src[i]; ++i) {
      dst[i] = src[i];
    }
  }
  dst[i] = '\0';
}

// --- Label Implementation ---
Label::Label(int32_t x, int32_t y, const char *initial_text, uint32_t color)
    : Widget(x, y, 0, 8), color_(color) {
  copy_str(text_, initial_text, sizeof(text_));
  size_t l = 0;
  for (const char *p = text_; *p; ++p)
    l++;
  w_ = static_cast<int32_t>(l) * 8;
}

void Label::set_text(const char *new_text) {
  if (strcmp(text_, new_text) != 0) {
    copy_str(text_, new_text, sizeof(text_));
    size_t l = 0;
    for (const char *p = text_; *p; ++p)
      l++;
    w_ = static_cast<int32_t>(l) * 8;
    dirty_ = true;
  }
}

void Label::draw(Kernel::Graphics::VirtualFramebuffer &vfb) {
  Kernel::AppCommon::text(vfb, x_, y_, text_, color_);
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

  size_t len = 0;
  for (const char *p = label_; *p; ++p)
    len++;
  int32_t text_w = static_cast<int32_t>(len) * 8;
  int32_t tx = x_ + (w_ - text_w) / 2;
  int32_t ty = y_ + (h_ - 8) / 2;

  Kernel::AppCommon::text(vfb, tx, ty, label_, text_color_);
  dirty_ = false;
}

// --- Checkbox Implementation ---
Checkbox::Checkbox(int32_t x, int32_t y, const char *label,
                   bool initial_checked)
    : Widget(x, y, 14 + 8 + (label ? strlen(label) * 8 : 0), 14),
      checked_(initial_checked), hovered_(false), prev_left_pressed_(false) {
  copy_str(label_, label, sizeof(label_));
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
  int box_size = 14;
  vfb.fill_rounded_rect(x_, y_, box_size, box_size, 3, COLOR_SURFACE);
  vfb.draw_rounded_rect(x_, y_, box_size, box_size, 3,
                        hovered_ ? COLOR_BLUE : COLOR_BORDER);

  if (checked_) {
    vfb.fill_rounded_rect(x_ + 3, y_ + 3, box_size - 6, box_size - 6, 2,
                          COLOR_BLUE);
  }

  if (label_[0] != '\0') {
    Kernel::AppCommon::text(vfb, x_ + box_size + 8, y_ + (box_size - 8) / 2,
                            label_, COLOR_TEXT);
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
  Kernel::AppCommon::text(vfb, x_ + 10, y_ + (h_ / 2) - 4, status_text_,
                          text_color_);
  dirty_ = false;
}

// --- Text Input Implementation ---
TextInput::TextInput(int32_t x, int32_t y, int32_t w, int32_t h, char *buf,
                     size_t max_len)
    : Widget(x, y, w, h), buffer_(buf), max_len_(max_len), len_(0),
      cursor_pos_(0), focused_(false) {
  if (buffer_) {
    len_ = strlen(buffer_);
    cursor_pos_ = len_;
  }
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
      int rel_x = mx - (x_ + 8);
      rel_x = std::max(rel_x, 0);
      size_t clicked_char = static_cast<size_t>(rel_x + 4) / 8;
      size_t new_cursor = clicked_char > len_ ? len_ : clicked_char;
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
    buffer_[len_] = '\0';
    dirty_ = true;
    return true;
  }
  return false;
}

void TextInput::draw(Kernel::Graphics::VirtualFramebuffer &vfb) {
  vfb.fill_rounded_rect(x_, y_, w_, h_, 4, COLOR_SURFACE);
  vfb.draw_rounded_rect(x_, y_, w_, h_, 4,
                        focused_ ? COLOR_FOCUS : COLOR_BORDER);

  if (buffer_) {
    Kernel::AppCommon::text(vfb, x_ + 8, y_ + (h_ - 8) / 2, buffer_,
                            COLOR_TEXT);
  }

  if (focused_) {
    size_t display_pos = cursor_pos_ > len_ ? len_ : cursor_pos_;
    char temp = buffer_[display_pos];
    buffer_[display_pos] = '\0';
    size_t prefix_len = strlen(buffer_);
    buffer_[display_pos] = temp;

    int32_t cursor_x = x_ + 8 + static_cast<int32_t>(prefix_len) * 8;
    vfb.fill_rect(cursor_x, y_ + 4, 2, h_ - 8, COLOR_BLUE);
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