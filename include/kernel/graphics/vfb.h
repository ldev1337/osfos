#pragma once
#include <cstdint>

#include <kernel/graphics/truetype.h>

namespace Kernel::Graphics {

class VirtualFramebuffer {
private:
  int32_t x_ = 0, y_ = 0;
  uint32_t width_ = 0, height_ = 0, pitch_ = 0;
  uint32_t *buffer_ = nullptr;
  bool initialized_ = false;
  bool owns_buffer_ = true;

public:
  VirtualFramebuffer() = default;
  VirtualFramebuffer(int32_t x, int32_t y, uint32_t width, uint32_t height);
  ~VirtualFramebuffer();

  VirtualFramebuffer(const VirtualFramebuffer &) = delete;
  VirtualFramebuffer &operator=(const VirtualFramebuffer &) = delete;

  VirtualFramebuffer(VirtualFramebuffer &&other) noexcept;
  VirtualFramebuffer &operator=(VirtualFramebuffer &&other) noexcept;

  bool init(int32_t x, int32_t y, uint32_t width, uint32_t height);
  bool open_window(uint32_t requested_width, uint32_t requested_height,
                   uint32_t min_width = 1, uint32_t min_height = 1);
  void destroy();

  bool valid() const { return initialized_ && buffer_ != nullptr; }
  bool has_focus() const;
  int32_t x() const { return x_; }
  int32_t y() const { return y_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t pitch() const { return pitch_; }
  uint32_t *buffer() { return buffer_; }
  const uint32_t *buffer() const { return buffer_; }
  bool attach(uint32_t *buffer, int32_t x, int32_t y, uint32_t width, uint32_t height);

  void flush() const;

  void put_pixel(uint32_t px, uint32_t py, uint32_t color);
  void clear(uint32_t color) { fill(color); }
  void fill(uint32_t color);
  void fill_rect(int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                 uint32_t color);
  void draw_rect(int32_t rx, int32_t ry, int32_t w, int32_t h, uint32_t color);
  void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                 uint32_t color);
  void draw_circle(int32_t xc, int32_t yc, int32_t r, uint32_t color);
  void fill_circle(int32_t xc, int32_t yc, int32_t r, uint32_t color);
  void fill_rect_alpha(int32_t rx, int32_t ry, int32_t w, int32_t h,
                       uint32_t color, uint8_t alpha);
  void fill_circle_alpha(int32_t xc, int32_t yc, int32_t r, uint32_t color,
                         uint8_t alpha);

  void draw_glyph(int32_t gx, int32_t gy, char ch, uint32_t color);
  void draw_glyph_bg(int32_t gx, int32_t gy, char ch, uint32_t fg, uint32_t bg);
  int draw_string(int32_t x, int32_t y, float font_size_pixels, const char *text,
                  uint32_t color);

  void blit(const uint32_t *src, uint32_t bx, uint32_t by, uint32_t bw,
            uint32_t bh, uint32_t src_pitch);
  void blit_transparent(const uint32_t *src, uint32_t bx, uint32_t by,
                        uint32_t bw, uint32_t bh, uint32_t src_pitch,
                        uint32_t transparent_color);
  void blit_alpha(const uint32_t *src, uint32_t bx, uint32_t by, uint32_t bw,
                  uint32_t bh, uint32_t src_pitch, uint8_t global_alpha);
  void draw_arc(int32_t xc, int32_t yc, int32_t r, int32_t start_angle,
                int32_t end_angle, uint32_t color);
  void draw_semicircle(int32_t xc, int32_t yc, int32_t r, int32_t angle_deg,
                       uint32_t color) {
    draw_arc(xc, yc, r, angle_deg, angle_deg + 180, color);
  }
  void fill_sector(int32_t xc, int32_t yc, int32_t r, int32_t start_angle,
                   int32_t end_angle, uint32_t color);
  void draw_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2,
                     int32_t y2, uint32_t color) {
    draw_line(x0, y0, x1, y1, color);
    draw_line(x1, y1, x2, y2, color);
    draw_line(x2, y2, x0, y0, color);
  }
  void fill_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2,
                     int32_t y2, uint32_t color);
  void draw_rounded_rect(int32_t rx, int32_t ry, int32_t w, int32_t h,
                         int32_t radius, uint32_t color);
  void fill_rounded_rect(int32_t rx, int32_t ry, int32_t w, int32_t h,
                         int32_t radius, uint32_t color);

  int draw_text_truetype(int32_t x, int32_t y, float font_size_pixels, TrueTypeFont &font, const char *text, uint32_t color);
};

} // namespace Kernel::Graphics