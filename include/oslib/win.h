#pragma once

#include <cstdint>
#include <kernel/graphics/vfb.h>

namespace oslib {

// Application-facing window/surface API. Applications should use Win rather
// than depending on Kernel::Graphics::VirtualFramebuffer directly.
class Win {
private:
  Kernel::Graphics::VirtualFramebuffer surface_;

public:
  Win() = default;
  ~Win() = default;
  Win(const Win &) = delete;
  Win &operator=(const Win &) = delete;
  Win(Win &&) noexcept = default;
  Win &operator=(Win &&) noexcept = default;

  bool open(uint32_t width, uint32_t height, uint32_t min_width = 1,
            uint32_t min_height = 1) {
    return surface_.open_window(width, height, min_width, min_height);
  }
  bool open_screen(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    return surface_.init(x, y, width, height);
  }
  bool attach_pixels(uint32_t *pixels, int32_t x, int32_t y, uint32_t width,
                     uint32_t height) {
    return surface_.attach(pixels, x, y, width, height);
  }
  void close() { surface_.destroy(); }

  bool valid() const { return surface_.valid(); }
  bool has_focus() const { return surface_.has_focus(); }
  uint32_t width() const { return surface_.width(); }
  uint32_t height() const { return surface_.height(); }
  uint32_t pitch() const { return surface_.pitch(); }
  int32_t x() const { return surface_.x(); }
  int32_t y() const { return surface_.y(); }
  uint32_t *pixels() { return surface_.buffer(); }
  const uint32_t *pixels() const { return surface_.buffer(); }

  void flush() const { surface_.flush(); }
  bool present_pixels(const uint32_t *pixels, uint32_t width, uint32_t height,
                      uint32_t pitch) const;
  bool exists() const;
  void unregister();

  void put_pixel(int32_t x, int32_t y, uint32_t color) {
    surface_.put_pixel(x, y, color);
  }
  void clear(uint32_t color) { surface_.clear(color); }
  void fill(uint32_t color) { surface_.fill(color); }
  void fill_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                 uint32_t color) {
    surface_.fill_rect(x, y, width, height, color);
  }
  void draw_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                 uint32_t color) {
    surface_.draw_rect(x, y, width, height, color);
  }
  void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                 uint32_t color) {
    surface_.draw_line(x0, y0, x1, y1, color);
  }
  void draw_circle(int32_t x, int32_t y, int32_t radius, uint32_t color) {
    surface_.draw_circle(x, y, radius, color);
  }
  void fill_circle(int32_t x, int32_t y, int32_t radius, uint32_t color) {
    surface_.fill_circle(x, y, radius, color);
  }
  void fill_rect_alpha(int32_t x, int32_t y, int32_t width, int32_t height,
                       uint32_t color, uint8_t alpha) {
    surface_.fill_rect_alpha(x, y, width, height, color, alpha);
  }
  void fill_circle_alpha(int32_t x, int32_t y, int32_t radius, uint32_t color,
                         uint8_t alpha) {
    surface_.fill_circle_alpha(x, y, radius, color, alpha);
  }
  int draw_string(int32_t x, int32_t y, float font_size_pixels,
                  const char *text, uint32_t color) {
    return surface_.draw_string(x, y, font_size_pixels, text, color);
  }
  int draw_text_truetype(int32_t x, int32_t y, float font_size_pixels,
                         Kernel::Graphics::TrueTypeFont &font,
                         const char *text, uint32_t color) {
    return surface_.draw_text_truetype(x, y, font_size_pixels, font, text,
                                       color);
  }
  void draw_glyph(int32_t x, int32_t y, char ch, uint32_t color) {
    surface_.draw_glyph(x, y, ch, color);
  }
  void draw_glyph_bg(int32_t x, int32_t y, char ch, uint32_t fg,
                     uint32_t bg) {
    surface_.draw_glyph_bg(x, y, ch, fg, bg);
  }
  void blit(const uint32_t *src, int32_t x, int32_t y, uint32_t width,
            uint32_t height, uint32_t src_pitch) {
    surface_.blit(src, x, y, width, height, src_pitch);
  }
  void blit_transparent(const uint32_t *src, int32_t x, int32_t y,
                        uint32_t width, uint32_t height, uint32_t src_pitch,
                        uint32_t transparent_color) {
    surface_.blit_transparent(src, x, y, width, height, src_pitch,
                              transparent_color);
  }
  void blit_alpha(const uint32_t *src, int32_t x, int32_t y, uint32_t width,
                  uint32_t height, uint32_t src_pitch, uint8_t alpha) {
    surface_.blit_alpha(src, x, y, width, height, src_pitch, alpha);
  }
  void draw_arc(int32_t x, int32_t y, int32_t radius, int32_t start_angle,
                int32_t end_angle, uint32_t color) {
    surface_.draw_arc(x, y, radius, start_angle, end_angle, color);
  }
  void draw_semicircle(int32_t x, int32_t y, int32_t radius, int32_t angle,
                       uint32_t color) {
    surface_.draw_semicircle(x, y, radius, angle, color);
  }
  void fill_sector(int32_t x, int32_t y, int32_t radius, int32_t start_angle,
                   int32_t end_angle, uint32_t color) {
    surface_.fill_sector(x, y, radius, start_angle, end_angle, color);
  }
  void draw_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     int32_t x2, int32_t y2, uint32_t color) {
    surface_.draw_triangle(x0, y0, x1, y1, x2, y2, color);
  }
  void fill_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     int32_t x2, int32_t y2, uint32_t color) {
    surface_.fill_triangle(x0, y0, x1, y1, x2, y2, color);
  }
  void draw_rounded_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                         int32_t radius, uint32_t color) {
    surface_.draw_rounded_rect(x, y, width, height, radius, color);
  }
  void fill_rounded_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                         int32_t radius, uint32_t color) {
    surface_.fill_rounded_rect(x, y, width, height, radius, color);
  }
};

using Window = Win;

} // namespace oslib
