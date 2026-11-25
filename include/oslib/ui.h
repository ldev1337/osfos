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

struct Rect {
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
};

class FlexLayout {
public:
  static Rect center(Rect bounds, int32_t width, int32_t height);
  static Rect row_item(Rect bounds, uint32_t index, uint32_t count,
                       int32_t gap = 0);
  static Rect column_item(Rect bounds, uint32_t index, uint32_t count,
                          int32_t gap = 0);
  static Rect grid_cell(Rect bounds, uint32_t row, uint32_t column,
                        uint32_t rows, uint32_t columns, int32_t gap_x = 0,
                        int32_t gap_y = 0);
};

// Base Stateful Widget Class with Dirty Tracking
class Widget {
protected:
  int32_t x_, y_, w_, h_;
  float font_size_pixels_;
  bool visible_;
  bool enabled_;
  bool dirty_;

public:
  Widget(int32_t x, int32_t y, int32_t w, int32_t h)
      : x_(x), y_(y), w_(w), h_(h), font_size_pixels_(14.0f), visible_(true),
        enabled_(true), dirty_(true) {}
  virtual ~Widget() = default;

  virtual void draw(Kernel::Graphics::VirtualFramebuffer &vfb) = 0;
  virtual bool handle_mouse(int mx, int my, bool left_pressed,
                            bool left_released) = 0;
  virtual bool handle_key(char c) = 0;

  bool is_dirty() const { return dirty_; }
  bool is_visible() const { return visible_; }
  bool is_enabled() const { return enabled_; }
  void set_bounds(Rect bounds);
  void set_visible(bool visible);
  void set_enabled(bool enabled);
  virtual void set_font_size_pixels(float font_size_pixels);
  float get_font_size_pixels() const { return font_size_pixels_; }
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
  void set_font_size_pixels(float font_size_pixels) override;
  void set_text(const char *new_text);
  void draw(Kernel::Graphics::VirtualFramebuffer &vfb) override;
  bool handle_mouse(int mx, int my, bool left_pressed,
                    bool left_released) override;
  bool handle_key(char c) override;
};

using TextObject = Label;

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
  void set_font_size_pixels(float font_size_pixels) override;
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
  size_t first_visible_char_;
  bool focused_;

public:
  TextInput(int32_t x, int32_t y, int32_t w, int32_t h, char *buf,
            size_t max_len);
  void set_text(const char *text);
  bool is_focused() const { return focused_; }
  void set_focused(bool focused);
  void draw(Kernel::Graphics::VirtualFramebuffer &vfb) override;
  bool handle_mouse(int mx, int my, bool left_pressed,
                    bool left_released) override;
  bool handle_key(char c) override;
};

class TextArea : public Widget {
private:
  const char *text_;
  size_t scroll_line_;
  bool focused_;
  uint32_t background_color_;
  uint32_t text_color_;
  uint32_t border_color_;

public:
  TextArea(int32_t x, int32_t y, int32_t w, int32_t h,
           const char *initial_text = "", uint32_t background = COLOR_BG,
           uint32_t text_color = COLOR_TEXT,
           uint32_t border_color = COLOR_BORDER);
  void set_text(const char *text);
  void set_focused(bool focused);
  void set_scroll_line(size_t line);
  size_t get_scroll_line() const { return scroll_line_; }
  void draw(Kernel::Graphics::VirtualFramebuffer &vfb) override;
  bool handle_mouse(int mx, int my, bool left_pressed,
                    bool left_released) override;
  bool handle_key(char c) override;
};

class WidgetGroup {
public:
  static constexpr size_t CAPACITY = 32;

private:
  Widget *widgets_[CAPACITY]{};
  size_t count_ = 0;

public:
  bool add(Widget &widget);
  void clear();
  size_t size() const { return count_; }
  void draw(Kernel::Graphics::VirtualFramebuffer &vfb,
            bool dirty_only = true);
  Widget *handle_mouse(int mx, int my, bool left_pressed, bool left_released);
  bool handle_key(char c);
};

void text(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x, int32_t y,
          const char *value, uint32_t color,
          float font_size_pixels = 14.0f);
int text(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x, int32_t y,
         float font_size_pixels, Kernel::Graphics::TrueTypeFont &font,
         const char *value, uint32_t color);
int text_width(const char *value, float font_size_pixels = 14.0f);
void number(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x, int32_t y,
            int32_t value, uint32_t color);
int mouse_x();
int mouse_y();
bool primary_pressed();

// 7. Interactive Modal Dialog Prompt
bool prompt_filename(Kernel::Graphics::VirtualFramebuffer &vfb,
                     const char *prompt, char *buf, size_t max_len);

} // namespace Kernel::UICommon