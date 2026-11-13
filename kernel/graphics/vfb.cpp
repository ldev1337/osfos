#include <arch/x86_64/string.h>
#include <algorithm>
#include <kernel/drivers/framebuffer.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/open_font.h>
#include <kernel/graphics/simplefonts.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/math.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>
#include <kernel/graphics/truetype.h>

namespace Kernel::Graphics {
namespace {

bool clip_rect(int32_t &x, int32_t &y, int32_t &w, int32_t &h, uint32_t width,
               uint32_t height) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > static_cast<int32_t>(width))
    w = static_cast<int32_t>(width) - x;
  if (y + h > static_cast<int32_t>(height))
    h = static_cast<int32_t>(height) - y;
  return w > 0 && h > 0;
}

bool clip_blit(uint32_t &x, uint32_t &y, uint32_t &w, uint32_t &h,
               uint32_t width, uint32_t height) {
  if (x >= width || y >= height)
    return false;
  if (w > width - x)
    w = width - x;
  if (h > height - y)
    h = height - y;
  return w && h;
}

void raster_glyph(uint32_t *buffer, uint32_t width, uint32_t height, int32_t x,
                  int32_t y, char ch, uint32_t fg, uint32_t bg, bool opaque) {
  unsigned char c = static_cast<unsigned char>(ch);
  if (c < 32 || c > 127)
    c = '?';
  const uint8_t *bits = Kernel::Graphics::Font::Basic8x8[c - 32];
  for (int32_t py = 0; py < 8; ++py) {
    const int32_t cy = y + py;
    if (cy < 0 || cy >= static_cast<int32_t>(height))
      continue;
    for (int32_t px = 0; px < 8; ++px) {
      const int32_t cx = x + px;
      if (cx < 0 || cx >= static_cast<int32_t>(width))
        continue;
      const bool set = bits[py] & (1u << px);
      if (set || opaque)
        buffer[static_cast<size_t>(cy) * width + cx] = set ? fg : bg;
    }
  }
}

uint32_t blend_color(uint32_t foreground, uint32_t background,
                     uint32_t alpha) {
  if (alpha >= 255)
    return foreground;
  const uint32_t inverse = 255 - alpha;
  const uint32_t red = (((foreground >> 16) & 255) * alpha +
                        ((background >> 16) & 255) * inverse + 127) /
                       255;
  const uint32_t green = (((foreground >> 8) & 255) * alpha +
                          ((background >> 8) & 255) * inverse + 127) /
                         255;
  const uint32_t blue = ((foreground & 255) * alpha +
                         (background & 255) * inverse + 127) /
                        255;
  return 0xFF000000 | red << 16 | green << 8 | blue;
}

void put_coverage(uint32_t *buffer, uint32_t width, int32_t x, int32_t y,
                  uint32_t color, uint32_t samples, uint32_t alpha = 255) {
  if (!samples || x < 0 || y < 0)
    return;
  const uint32_t coverage = (samples * alpha + 2) / 4;
  uint32_t &pixel = buffer[static_cast<size_t>(y) * width + x];
  pixel = blend_color(color, pixel, coverage);
}

bool line_sample_inside(int64_t px, int64_t py, int64_t ax, int64_t ay,
                        int64_t bx, int64_t by) {
  const int64_t dx = bx - ax;
  const int64_t dy = by - ay;
  const int64_t length_squared = dx * dx + dy * dy;
  if (!length_squared) {
    const int64_t ox = px - ax;
    const int64_t oy = py - ay;
    return ox * ox + oy * oy <= 4;
  }
  const int64_t projection = (px - ax) * dx + (py - ay) * dy;
  if (projection <= 0) {
    const int64_t ox = px - ax;
    const int64_t oy = py - ay;
    return ox * ox + oy * oy <= 4;
  }
  if (projection >= length_squared) {
    const int64_t ox = px - bx;
    const int64_t oy = py - by;
    return ox * ox + oy * oy <= 4;
  }
  const int64_t cross = (px - ax) * dy - (py - ay) * dx;
  return cross * cross <= 4 * length_squared;
}

void raster_circle_2x(uint32_t *buffer, uint32_t width, uint32_t height,
                      int32_t xc, int32_t yc, int32_t radius, uint32_t color,
                      uint8_t alpha, bool outline) {
  if (radius < 0)
    return;
  if (radius == 0) {
    if (static_cast<uint32_t>(xc) < width &&
        static_cast<uint32_t>(yc) < height)
      put_coverage(buffer, width, xc, yc, color, 4, alpha);
    return;
  }
  const int32_t min_x = static_cast<int32_t>(
      std::max<int64_t>(static_cast<int64_t>(xc) - radius - 1, 0));
  const int32_t max_x = static_cast<int32_t>(std::min<int64_t>(
      static_cast<int64_t>(xc) + radius + 1, static_cast<int64_t>(width) - 1));
  const int32_t min_y = static_cast<int32_t>(
      std::max<int64_t>(static_cast<int64_t>(yc) - radius - 1, 0));
  const int32_t max_y = static_cast<int32_t>(std::min<int64_t>(
      static_cast<int64_t>(yc) + radius + 1, static_cast<int64_t>(height) - 1));
  if (min_x > max_x || min_y > max_y)
    return;
  const int64_t center_x = static_cast<int64_t>(xc) * 4 + 2;
  const int64_t center_y = static_cast<int64_t>(yc) * 4 + 2;
  const int64_t outer_radius = static_cast<int64_t>(radius) * 4 + 2;
  const int64_t inner_radius =
      std::max(static_cast<int64_t>(radius) * 4 - 2, int64_t(0));
  const int64_t outer_squared = outer_radius * outer_radius;
  const int64_t inner_squared = inner_radius * inner_radius;

  for (int32_t y = min_y; y <= max_y; ++y) {
    for (int32_t x = min_x; x <= max_x; ++x) {
      uint32_t covered = 0;
      for (int32_t sy : {1, 3}) {
        for (int32_t sx : {1, 3}) {
          const int64_t dx = static_cast<int64_t>(x) * 4 + sx - center_x;
          const int64_t dy = static_cast<int64_t>(y) * 4 + sy - center_y;
          const int64_t distance_squared = dx * dx + dy * dy;
              const bool inside = outline
                      ? distance_squared <= outer_squared &&
                        distance_squared >= inner_squared
                      : distance_squared <=
                        static_cast<int64_t>(radius) * 4 *
                        (static_cast<int64_t>(radius) * 4);
          covered += inside;
        }
      }
      put_coverage(buffer, width, x, y, color, covered, alpha);
    }
  }
}

template <typename Pixel>
void blit_pixels(uint32_t *buffer, uint32_t width, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h, const uint32_t *src,
                 uint32_t src_pitch, Pixel pixel) {
  const uint8_t *src_row = reinterpret_cast<const uint8_t *>(src);
  uint32_t *dst_row = buffer + y * width + x;
  for (uint32_t row = 0; row < h; ++row) {
    const auto *source = reinterpret_cast<const uint32_t *>(src_row);
    for (uint32_t col = 0; col < w; ++col)
      dst_row[col] = pixel(source[col], dst_row[col]);
    dst_row += width;
    src_row += src_pitch;
  }
}

template <typename Draw>
void walk_arc(int32_t xc, int32_t yc, int32_t r, int32_t start, int32_t end,
              Draw draw) {
  bool first = true;
  int32_t prev_x = 0, prev_y = 0;
  for (int32_t deg = start; deg <= end; ++deg) {
    const int32_t x = xc + r * Kernel::Lib::Math::fixed_cos(deg) / 1024;
    const int32_t y = yc + r * Kernel::Lib::Math::fixed_sin(deg) / 1024;
    if (!first)
      draw(prev_x, prev_y, x, y);
    prev_x = x;
    prev_y = y;
    first = false;
  }
}
} // namespace

VirtualFramebuffer::VirtualFramebuffer(int32_t x, int32_t y, uint32_t width,
                                       uint32_t height) {
  init(x, y, width, height);
}

VirtualFramebuffer::~VirtualFramebuffer() { destroy(); }

VirtualFramebuffer::VirtualFramebuffer(VirtualFramebuffer &&other) noexcept {
  *this = static_cast<VirtualFramebuffer &&>(other);
}

VirtualFramebuffer &
VirtualFramebuffer::operator=(VirtualFramebuffer &&other) noexcept {
  if (this != &other) {
    destroy();
    x_ = other.x_;
    y_ = other.y_;
    width_ = other.width_;
    height_ = other.height_;
    buffer_ = other.buffer_;
    pitch_ = other.pitch_;
    initialized_ = other.initialized_;
    owns_buffer_ = other.owns_buffer_;
    other.buffer_ = nullptr;
    other.owns_buffer_ = true;
    other.initialized_ = false;
    other.width_ = other.height_ = 0;
  }
  return *this;
}

bool VirtualFramebuffer::init(int32_t x, int32_t y, uint32_t width,
                              uint32_t height) {
  destroy();
  if (width == 0 || height == 0)
    return false;

  x_ = x;
  y_ = y;
  width_ = width;
  height_ = height;
  pitch_ = width * sizeof(uint32_t);

  size_t size = static_cast<size_t>(height_) * pitch_;
  buffer_ = static_cast<uint32_t *>(kmalloc(size));
  if (!buffer_)
    return false;

  memset(buffer_, 0, size);
  initialized_ = true;
  return true;
}

bool VirtualFramebuffer::attach(uint32_t *buffer, int32_t x, int32_t y,
                                uint32_t width, uint32_t height) {
  destroy();
  if (!buffer || width == 0 || height == 0)
    return false;
  x_ = x;
  y_ = y;
  width_ = width;
  height_ = height;
  pitch_ = width * sizeof(uint32_t);
  buffer_ = buffer;
  owns_buffer_ = false;
  initialized_ = true;
  return true;
}

bool VirtualFramebuffer::open_window(uint32_t requested_width,
                                     uint32_t requested_height,
                                     uint32_t min_width, uint32_t min_height) {
  (void)min_width;
  (void)min_height;
  uint32_t width = 0, height = 0;
  if (!Kernel::Graphics::WindowManager::request_window_size(
          Kernel::Scheduler::current_thread_id(), requested_width,
          requested_height, &width, &height)) {
    destroy();
    return false;
  }
  return init(0, 0, width, height);
}

bool VirtualFramebuffer::has_focus() const {
  return Kernel::Graphics::WindowManager::has_keyboard_focus(
      Kernel::Scheduler::current_thread_id());
}

void VirtualFramebuffer::destroy() {
  if (buffer_ && initialized_ && owns_buffer_)
    kfree(buffer_);
  buffer_ = nullptr;
  owns_buffer_ = true;
  initialized_ = false;
  width_ = 0;
  height_ = 0;
}

void VirtualFramebuffer::flush() const {
  if (!initialized_ || !buffer_)
    return;
  if (Kernel::Graphics::WindowManager::present(
          Kernel::Scheduler::current_thread_id(), buffer_, width_, height_,
          pitch_))
    return;
  Kernel::Drivers::Framebuffer::blit(buffer_, static_cast<uint32_t>(x_),
                                     static_cast<uint32_t>(y_), width_, height_,
                                     pitch_);
}

void VirtualFramebuffer::put_pixel(uint32_t px, uint32_t py, uint32_t color) {
  if (!initialized_ || px >= width_ || py >= height_) [[unlikely]]
    return;
  buffer_[py * width_ + px] = color;
}

void VirtualFramebuffer::fill(uint32_t color) {
  if (!initialized_ || !buffer_)
    return;
  fill_rect(0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_),
            color);
}

void VirtualFramebuffer::fill_rect(int32_t rx, int32_t ry, int32_t rw,
                                   int32_t rh, uint32_t color) {
  if (!initialized_ || !buffer_)
    return;
  if (!clip_rect(rx, ry, rw, rh, width_, height_))
    return;

  const uint32_t ux = static_cast<uint32_t>(rx);
  const uint32_t uy = static_cast<uint32_t>(ry);
  const uint32_t uw = static_cast<uint32_t>(rw);
  const uint32_t uh = static_cast<uint32_t>(rh);

  for (uint32_t py = 0; py < uh; ++py) {
    uint32_t *row = buffer_ + (uy + py) * width_ + ux;
    uint32_t px = 0;
    for (; px + 1 < uw; px += 2)
      *reinterpret_cast<uint64_t *>(row + px) =
          uint64_t(color) | (uint64_t(color) << 32);
    if (px < uw)
      row[px] = color;
  }
}

void VirtualFramebuffer::draw_rect(int32_t rx, int32_t ry, int32_t w, int32_t h,
                                   uint32_t color) {
  if (w <= 0 || h <= 0)
    return;
  fill_rect(rx, ry, w, 1, color);
  fill_rect(rx, ry + h - 1, w, 1, color);
  fill_rect(rx, ry, 1, h, color);
  fill_rect(rx + w - 1, ry, 1, h, color);
}

void VirtualFramebuffer::draw_line(int32_t x0, int32_t y0, int32_t x1,
                                   int32_t y1, uint32_t color) {
  if (!initialized_)
    return;
  const int32_t min_x = static_cast<int32_t>(std::max<int64_t>(
      std::min<int64_t>(x0, x1) - 1, 0));
  const int32_t max_x = static_cast<int32_t>(std::min<int64_t>(
      std::max<int64_t>(x0, x1) + 1, static_cast<int64_t>(width_) - 1));
  const int32_t min_y = static_cast<int32_t>(std::max<int64_t>(
      std::min<int64_t>(y0, y1) - 1, 0));
  const int32_t max_y = static_cast<int32_t>(std::min<int64_t>(
      std::max<int64_t>(y0, y1) + 1, static_cast<int64_t>(height_) - 1));
  if (min_x > max_x || min_y > max_y)
    return;

  const int64_t ax = static_cast<int64_t>(x0) * 4 + 2;
  const int64_t ay = static_cast<int64_t>(y0) * 4 + 2;
  const int64_t bx = static_cast<int64_t>(x1) * 4 + 2;
  const int64_t by = static_cast<int64_t>(y1) * 4 + 2;
  for (int32_t y = min_y; y <= max_y; ++y) {
    for (int32_t x = min_x; x <= max_x; ++x) {
      uint32_t covered = 0;
      for (int32_t sy : {1, 3})
        for (int32_t sx : {1, 3})
          covered += line_sample_inside(static_cast<int64_t>(x) * 4 + sx,
                                        static_cast<int64_t>(y) * 4 + sy, ax,
                                        ay, bx, by);
      put_coverage(buffer_, width_, x, y, color, covered);
    }
  }
}

void VirtualFramebuffer::draw_circle(int32_t xc, int32_t yc, int32_t r,
                                     uint32_t color) {
  if (!initialized_ || !buffer_)
    return;
  raster_circle_2x(buffer_, width_, height_, xc, yc, r, color, 255, true);
}

void VirtualFramebuffer::fill_circle(int32_t xc, int32_t yc, int32_t r,
                                     uint32_t color) {
  if (!initialized_ || !buffer_)
    return;
  raster_circle_2x(buffer_, width_, height_, xc, yc, r, color, 255, false);
}

inline uint32_t blend_pixel_32(uint32_t fg, uint32_t bg, uint8_t alpha) {
  if (alpha == 255)
    return fg;
  if (alpha == 0)
    return bg;
  uint8_t fr = (fg >> 16) & 0xFF;
  uint8_t fg_g = (fg >> 8) & 0xFF;
  uint8_t fb_c = fg & 0xFF;
  uint8_t br = (bg >> 16) & 0xFF;
  uint8_t bg_g = (bg >> 8) & 0xFF;
  uint8_t bb = bg & 0xFF;

  uint8_t inv = 255 - alpha;
  uint8_t or_c = (fr * alpha + br * inv + 128) >> 8;
  uint8_t og_c = (fg_g * alpha + bg_g * inv + 128) >> 8;
  uint8_t ob_c = (fb_c * alpha + bb * inv + 128) >> 8;

  return (uint32_t(or_c) << 16) | (uint32_t(og_c) << 8) | ob_c;
}

void VirtualFramebuffer::fill_rect_alpha(int32_t rx, int32_t ry, int32_t w,
                                         int32_t h, uint32_t color,
                                         uint8_t alpha) {
  if (alpha == 0)
    return;
  if (alpha == 255) {
    fill_rect(rx, ry, w, h, color);
    return;
  }
  if (!initialized_ || !clip_rect(rx, ry, w, h, width_, height_))
    return;

  for (int32_t py = 0; py < h; ++py) {
    uint32_t *row = buffer_ + (ry + py) * width_ + rx;
    for (int32_t px = 0; px < w; ++px) {
      row[px] = blend_pixel_32(color, row[px], alpha);
    }
  }
}

void VirtualFramebuffer::fill_circle_alpha(int32_t xc, int32_t yc, int32_t r,
                                           uint32_t color, uint8_t alpha) {
  if (!initialized_ || !buffer_ || alpha == 0)
    return;
  raster_circle_2x(buffer_, width_, height_, xc, yc, r, color, alpha, false);
}

namespace {

constexpr float kDefaultGlyphFontSize = 16.0f;
constexpr int kDefaultGlyphBgHeight = 18;

TrueTypeFont &default_ttf_font() {
  static TrueTypeFont font;
  static bool initialized = false;
  if (!initialized) {
    initialized = OpenFont::load_preferred(font, "GeistMono.ttf", "Geist.ttf");
  }
  return font;
}

bool render_default_glyph_ttf(VirtualFramebuffer &fb, int32_t gx, int32_t gy,
                             char ch, uint32_t fg, uint32_t bg,
                             bool fill_background) {
  TrueTypeFont &font = default_ttf_font();
  if (!font.is_valid())
    return false;

  char text[2] = {ch, '\0'};
  if (fill_background) {
    const int width = font.get_text_width(kDefaultGlyphFontSize, text);
    if (width > 0)
      fb.fill_rect(gx, gy, width, kDefaultGlyphBgHeight, bg);
  }

  fb.draw_text_truetype(gx, gy, kDefaultGlyphFontSize, font, text, fg);
  return true;
}

} // namespace

int VirtualFramebuffer::draw_string(int32_t x, int32_t y,
                                   float font_size_pixels, const char *text,
                                   uint32_t color) {
  if (!initialized_ || !text || font_size_pixels <= 0.0f)
    return x;

  TrueTypeFont &font = default_ttf_font();
  if (font.is_valid()) {
    return draw_text_truetype(x, y, font_size_pixels, font, text, color);
  }

  int cursor = x;
  int glyph_size = static_cast<int>(font_size_pixels + 0.5f);
  if (glyph_size < 1)
    glyph_size = 1;
  for (const char *p = text; *p; ++p) {
    unsigned char ch = static_cast<unsigned char>(*p);
    if (ch < 32 || ch > 127)
      ch = '?';
    const uint8_t *bits = Kernel::Graphics::Font::Basic8x8[ch - 32];
    for (int32_t py = 0; py < glyph_size; ++py) {
      const int32_t source_y = py * 8 / glyph_size;
      const int32_t target_y = y + py;
      if (target_y < 0 || target_y >= static_cast<int32_t>(height_))
        continue;
      for (int32_t px = 0; px < glyph_size; ++px) {
        const int32_t source_x = px * 8 / glyph_size;
        const int32_t target_x = cursor + px;
        if ((bits[source_y] & (1u << source_x)) && target_x >= 0 &&
            target_x < static_cast<int32_t>(width_))
          buffer_[static_cast<size_t>(target_y) * width_ + target_x] = color;
      }
    }
    cursor += glyph_size;
  }
  return cursor;
}

void VirtualFramebuffer::draw_glyph(int32_t gx, int32_t gy, char ch,
                                    uint32_t color) {
  if (!initialized_)
    return;
  if (render_default_glyph_ttf(*this, gx, gy, ch, color, 0, false))
    return;
  raster_glyph(buffer_, width_, height_, gx, gy, ch, color, 0, false);
}

void VirtualFramebuffer::draw_glyph_bg(int32_t gx, int32_t gy, char ch,
                                       uint32_t fg, uint32_t bg) {
  if (!initialized_)
    return;
  if (render_default_glyph_ttf(*this, gx, gy, ch, fg, bg, true))
    return;
  raster_glyph(buffer_, width_, height_, gx, gy, ch, fg, bg, true);
}

void VirtualFramebuffer::blit(const uint32_t *src, uint32_t bx, uint32_t by,
                              uint32_t bw, uint32_t bh, uint32_t src_pitch) {
  if (!initialized_ || !src || !clip_blit(bx, by, bw, bh, width_, height_))
    return;
  const uint8_t *source = reinterpret_cast<const uint8_t *>(src);
  uint32_t *destination = buffer_ + by * width_ + bx;
  for (uint32_t row = 0; row < bh;
       ++row, destination += width_, source += src_pitch)
    memcpy(destination, source, static_cast<size_t>(bw) * sizeof(uint32_t));
}

void VirtualFramebuffer::blit_transparent(const uint32_t *src, uint32_t bx,
                                          uint32_t by, uint32_t bw, uint32_t bh,
                                          uint32_t src_pitch,
                                          uint32_t transparent_color) {
  if (!initialized_ || !src || !clip_blit(bx, by, bw, bh, width_, height_))
    return;
  blit_pixels(buffer_, width_, bx, by, bw, bh, src, src_pitch,
              [transparent_color](uint32_t src_pixel, uint32_t dst_pixel) {
                return src_pixel == transparent_color ? dst_pixel : src_pixel;
              });
}

void VirtualFramebuffer::blit_alpha(const uint32_t *src, uint32_t bx,
                                    uint32_t by, uint32_t bw, uint32_t bh,
                                    uint32_t src_pitch, uint8_t global_alpha) {
  if (global_alpha == 0 || !initialized_ || !src ||
      !clip_blit(bx, by, bw, bh, width_, height_))
    return;
  blit_pixels(buffer_, width_, bx, by, bw, bh, src, src_pitch,
              [global_alpha](uint32_t src_pixel, uint32_t dst_pixel) {
                return blend_pixel_32(src_pixel, dst_pixel, global_alpha);
              });
}

void VirtualFramebuffer::draw_arc(int32_t xc, int32_t yc, int32_t r,
                                  int32_t start_angle, int32_t end_angle,
                                  uint32_t color) {
  if (!initialized_ || r < 0)
    return;
  walk_arc(xc, yc, r, start_angle, end_angle,
           [this, color](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
             draw_line(x0, y0, x1, y1, color);
           });
}

void VirtualFramebuffer::fill_sector(int32_t xc, int32_t yc, int32_t r,
                                     int32_t start_angle, int32_t end_angle,
                                     uint32_t color) {
  if (!initialized_ || r < 0)
    return;
  walk_arc(
      xc, yc, r, start_angle, end_angle,
      [this, xc, yc, color](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        fill_triangle(xc, yc, x0, y0, x1, y1, color);
      });
}

void VirtualFramebuffer::fill_triangle(int32_t x0, int32_t y0, int32_t x1,
                                       int32_t y1, int32_t x2, int32_t y2,
                                       uint32_t color) {
  if (!initialized_)
    return;
  if (!buffer_)
    return;
  const int64_t ax = static_cast<int64_t>(x0) * 4 + 2;
  const int64_t ay = static_cast<int64_t>(y0) * 4 + 2;
  const int64_t bx = static_cast<int64_t>(x1) * 4 + 2;
  const int64_t by = static_cast<int64_t>(y1) * 4 + 2;
  const int64_t cx = static_cast<int64_t>(x2) * 4 + 2;
  const int64_t cy = static_cast<int64_t>(y2) * 4 + 2;
  const int64_t area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  if (!area)
    return;

  const int32_t min_x = static_cast<int32_t>(std::max<int64_t>(
      std::min<int64_t>({x0, x1, x2}) - 1, 0));
  const int32_t max_x = static_cast<int32_t>(std::min<int64_t>(
      std::max<int64_t>({x0, x1, x2}) + 1, static_cast<int64_t>(width_) - 1));
  const int32_t min_y = static_cast<int32_t>(std::max<int64_t>(
      std::min<int64_t>({y0, y1, y2}) - 1, 0));
  const int32_t max_y = static_cast<int32_t>(std::min<int64_t>(
      std::max<int64_t>({y0, y1, y2}) + 1, static_cast<int64_t>(height_) - 1));
  if (min_x > max_x || min_y > max_y)
    return;

  const bool positive = area > 0;
  for (int32_t y = min_y; y <= max_y; ++y) {
    for (int32_t x = min_x; x <= max_x; ++x) {
      uint32_t covered = 0;
      for (int32_t sy : {1, 3}) {
        for (int32_t sx : {1, 3}) {
          const int64_t px = static_cast<int64_t>(x) * 4 + sx;
          const int64_t py = static_cast<int64_t>(y) * 4 + sy;
          const int64_t e0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
          const int64_t e1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
          const int64_t e2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
          covered += positive ? (e0 >= 0 && e1 >= 0 && e2 >= 0)
                              : (e0 <= 0 && e1 <= 0 && e2 <= 0);
        }
      }
      put_coverage(buffer_, width_, x, y, color, covered);
    }
  }
}

void VirtualFramebuffer::draw_rounded_rect(int32_t rx, int32_t ry, int32_t w,
                                           int32_t h, int32_t radius,
                                           uint32_t color) {
  if (radius <= 0) {
    draw_rect(rx, ry, w, h, color);
    return;
  }
  draw_line(rx + radius, ry, rx + w - radius - 1, ry, color);
  draw_line(rx + radius, ry + h - 1, rx + w - radius - 1, ry + h - 1, color);
  draw_line(rx, ry + radius, rx, ry + h - radius - 1, color);
  draw_line(rx + w - 1, ry + radius, rx + w - 1, ry + h - radius - 1, color);

  auto draw_arc_2x = [&](int32_t cx, int32_t cy, int start, int end) {
    const int32_t min_x = std::max(cx - radius - 1, 0);
    const int32_t max_x = std::min(cx + radius + 1,
                                   static_cast<int32_t>(width_) - 1);
    const int32_t min_y = std::max(cy - radius - 1, 0);
    const int32_t max_y = std::min(cy + radius + 1,
                                   static_cast<int32_t>(height_) - 1);
    const int64_t outer = static_cast<int64_t>(radius) * 4 + 2;
    const int64_t inner = std::max(static_cast<int64_t>(radius) * 4 - 2,
                                   int64_t(0));
    for (int32_t y = min_y; y <= max_y; ++y) {
      for (int32_t x = min_x; x <= max_x; ++x) {
        uint32_t covered = 0;
        for (int32_t sy : {1, 3}) {
          for (int32_t sx : {1, 3}) {
            const int64_t dx = static_cast<int64_t>(x) * 4 + sx -
                               static_cast<int64_t>(cx) * 4 - 2;
            const int64_t dy = static_cast<int64_t>(y) * 4 + sy -
                               static_cast<int64_t>(cy) * 4 - 2;
            const int64_t distance = dx * dx + dy * dy;
            if (distance > outer * outer || distance < inner * inner)
              continue;
            const int degrees = Kernel::Lib::Math::atan2(
                                    static_cast<float>(dy),
                                    static_cast<float>(dx)) *
                                Kernel::Lib::Math::RAD_TO_DEG;
            const int normalized = degrees < 0 ? degrees + 360 : degrees;
            const bool in_arc = start <= end
                                    ? normalized >= start && normalized <= end
                                    : normalized >= start || normalized <= end;
            covered += in_arc;
          }
        }
        put_coverage(buffer_, width_, x, y, color, covered);
      }
    }
  };

  draw_arc_2x(rx + radius, ry + radius, 180, 270);
  draw_arc_2x(rx + w - radius - 1, ry + radius, 270, 360);
  draw_arc_2x(rx + radius, ry + h - radius - 1, 90, 180);
  draw_arc_2x(rx + w - radius - 1, ry + h - radius - 1, 0, 90);
}

void VirtualFramebuffer::fill_rounded_rect(int32_t rx, int32_t ry, int32_t w,
                                           int32_t h, int32_t radius,
                                           uint32_t color) {
  if (w <= 0 || h <= 0)
    return;
  radius = std::min(radius, std::min(w / 2, h / 2));
  if (radius <= 0) {
    fill_rect(rx, ry, w, h, color);
    return;
  }
  fill_rect(rx + radius, ry, w - 2 * radius, h, color);
  fill_rect(rx, ry + radius, radius, h - 2 * radius, color);
  fill_rect(rx + w - radius, ry + radius, radius, h - 2 * radius, color);

  const int64_t radius_scaled = static_cast<int64_t>(radius) * 4;
  const int64_t radius_squared = radius_scaled * radius_scaled;
  auto draw_corner = [&](int32_t left, int32_t top, int32_t center_x,
                         int32_t center_y) {
    const int32_t min_x = std::max(left, 0);
    const int32_t max_x = std::min(left + radius - 1,
                                   static_cast<int32_t>(width_) - 1);
    const int32_t min_y = std::max(top, 0);
    const int32_t max_y = std::min(top + radius - 1,
                                   static_cast<int32_t>(height_) - 1);
    if (min_x > max_x || min_y > max_y)
      return;
    for (int32_t y = min_y; y <= max_y; ++y) {
      for (int32_t x = min_x; x <= max_x; ++x) {
        uint32_t covered = 0;
        for (int32_t sy : {1, 3}) {
          for (int32_t sx : {1, 3}) {
            const int64_t dx = static_cast<int64_t>(x) * 4 + sx - center_x;
            const int64_t dy = static_cast<int64_t>(y) * 4 + sy - center_y;
            covered += dx * dx + dy * dy <= radius_squared;
          }
        }
        put_coverage(buffer_, width_, x, y, color, covered);
      }
    }
  };

  draw_corner(rx, ry, (rx + radius) * 4, (ry + radius) * 4);
  draw_corner(rx + w - radius, ry, (rx + w - radius) * 4,
              (ry + radius) * 4);
  draw_corner(rx, ry + h - radius, (rx + radius) * 4,
              (ry + h - radius) * 4);
  draw_corner(rx + w - radius, ry + h - radius,
              (rx + w - radius) * 4, (ry + h - radius) * 4);
}

int VirtualFramebuffer::draw_text_truetype(int32_t x, int32_t y, float font_size_pixels, TrueTypeFont &font, const char *text, uint32_t color) {
    if (!initialized_ || !buffer_) return x;
    return font.render_text(buffer_, width_, height_, pitch_, x, y, font_size_pixels, text, color);
}

} // namespace Kernel::Graphics
