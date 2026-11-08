#pragma once

#include <cstddef>
#include <cstdint>
#include <kernel/graphics/vfb.h>

namespace Kernel::UICommon {

// Modern Theme Color Palette (Catppuccin / Dark Theme)
constexpr uint32_t COLOR_BG = 0xFF1E1E2E;
constexpr uint32_t COLOR_SURFACE = 0xFF313244;
constexpr uint32_t COLOR_OVERLAY = 0xFF45475A;
constexpr uint32_t COLOR_TEXT = 0xFFCDD6F4;
constexpr uint32_t COLOR_SUBTEXT = 0xFFA6ADC8;
constexpr uint32_t COLOR_BLUE = 0xFF89B4FA;
constexpr uint32_t COLOR_GREEN = 0xFFA6E3A1;
constexpr uint32_t COLOR_RED = 0xFFF38BA8;
constexpr uint32_t COLOR_YELLOW = 0xFFF9E2AF;
constexpr uint32_t COLOR_BORDER = 0xFF585B70;
constexpr uint32_t COLOR_FOCUS = 0xFF89B4FA;

// Base Stateful Widget Class with Dirty Tracking
class Widget {
protected:
  int32_t x_, y_, w_, h_;
  bool dirty_;

public:
  Widget(int32_t x, int32_t y, int32_t w, int32_t h)
      : x_(x), y_(y), w_(w), h_(h), dirty_(true) {}
  virtual ~Widget() = default;

  virtual void draw(Kernel::Graphics::VirtualFramebuffer &vfb) = 0;
  virtual bool handle_mouse(int mx, int my, bool left_pressed,
                            bool left_released) = 0;
  virtual bool handle_key(char c) = 0;

  bool is_dirty() const { return dirty_; }
  void mark_dirty() { dirty_ = true; }
  void clear_dirty() { dirty_ = false; }

  int32_t get_x() const { return x_; }
  int32_t get_y() const { return y_; }
  int32_t get_width() const { return w_; }
  int32_t get_height() const { return h_; }
};

// 1. Label Widget
class Label : public Widget {
private:
  char text_[64];
  uint32_t color_;

public:
  Label(int32_t x, int32_t y, const char *initial_text,
        uint32_t color = COLOR_TEXT);
  void set_text(const char *new_text);
  void draw(Kernel::Graphics::VirtualFramebuffer &vfb) override;
  bool handle_mouse(int mx, int my, bool left_pressed,
                    bool left_released) override;
  bool handle_key(char c) override;
};

// 2. Clickable Button Widget
class Button : public Widget {
private:
  char label_[32];
  uint32_t bg_color_;
  uint32_t text_color_;
  uint32_t border_color_;
  bool hovered_;
  bool pressed_;

public:
  Button(int32_t x, int32_t y, int32_t w, int32_t h, const char *label,
         uint32_t bg_color = COLOR_SURFACE, uint32_t text_color = COLOR_TEXT,
         uint32_t border_color = COLOR_BORDER);
  void draw(Kernel::Graphics::VirtualFramebuffer &vfb) override;
  bool handle_mouse(int mx, int my, bool left_pressed,
                    bool left_released) override;
  bool handle_key(char c) override;
};

// 3. Checkbox Widget
class Checkbox : public Widget {
private:
  char label_[32];
  bool checked_;
  bool hovered_;
  bool prev_left_pressed_;

public:
  Checkbox(int32_t x, int32_t y, const char *label,
           bool initial_checked = false);
  bool is_checked() const { return checked_; }
  void set_checked(bool checked);
  void draw(Kernel::Graphics::VirtualFramebuffer &vfb) override;
  bool handle_mouse(int mx, int my, bool left_pressed,
                    bool left_released) override;
  bool handle_key(char c) override;
};

// 4. Slider Widget
class Slider : public Widget {
private:
  int32_t min_val_;
  int32_t max_val_;
  int32_t val_;
  bool hovered_;
  bool dragging_;

public:
  Slider(int32_t x, int32_t y, int32_t w, int32_t h, int32_t min_val,
         int32_t max_val, int32_t initial_val);
  int32_t get_value() const { return val_; }
  void set_value(int32_t val);
  void draw(Kernel::Graphics::VirtualFramebuffer &vfb) override;
  bool handle_mouse(int mx, int my, bool left_pressed,
                    bool left_released) override;
  bool handle_key(char c) override;
};

// 5. Status Bar Widget
class StatusBar : public Widget {
private:
  char status_text_[128];
  uint32_t bg_color_;
  uint32_t text_color_;
  uint32_t border_color_;

public:
  StatusBar(int32_t x, int32_t y, int32_t w, int32_t h,
            const char *initial_text = "", uint32_t bg_color = COLOR_SURFACE,
            uint32_t text_color = COLOR_TEXT,
            uint32_t border_color = COLOR_BORDER);
  void set_status(const char *new_text);
  void draw(Kernel::Graphics::VirtualFramebuffer &vfb) override;
  bool handle_mouse(int mx, int my, bool left_pressed,
                    bool left_released) override;
  bool handle_key(char c) override;
};

// 6. Text Input Field Widget
class TextInput : public Widget {
private:
  char *buffer_;
  size_t max_len_;
  size_t len_;
  size_t cursor_pos_;
  bool focused_;

public:
  TextInput(int32_t x, int32_t y, int32_t w, int32_t h, char *buf,
            size_t max_len);
  bool is_focused() const { return focused_; }
  void set_focused(bool focused);
  void draw(Kernel::Graphics::VirtualFramebuffer &vfb) override;
  bool handle_mouse(int mx, int my, bool left_pressed,
                    bool left_released) override;
  bool handle_key(char c) override;
};

// 7. Interactive Modal Dialog Prompt
bool prompt_filename(Kernel::Graphics::VirtualFramebuffer &vfb,
                     const char *prompt, char *buf, size_t max_len);

} // namespace Kernel::UICommon