#include <arch/x86_64/string.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/init.h>

#include <algorithm>

namespace Kernel::Drivers::Framebuffer {

static Config fb{};
static bool initialized = false;

constexpr uint8_t bytes_per_pixel(Format f) {
  switch (f) {
  case Format::RGB24:
  case Format::BGR24:
    return 3;
  case Format::RGB32:
  case Format::BGR32:
    return 4;
  case Format::RGB565:
  case Format::BGR565:
    return 2;
  }
  return 0;
}

static inline uint32_t swap_red_blue(uint32_t color) {
  return (color & 0xFF00FF00u) | ((color & 0x000000FFu) << 16) |
         ((color & 0x00FF0000u) >> 16);
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, size_t size) {
  while (size >= sizeof(uint64_t)) {
    *reinterpret_cast<uint64_t *>(dst) =
        *reinterpret_cast<const uint64_t *>(src);
    dst += sizeof(uint64_t);
    src += sizeof(uint64_t);
    size -= sizeof(uint64_t);
  }
  while (size--)
    *dst++ = *src++;
}

bool __init init(const Config &config) {
  initialized = false;
  const uint8_t bpp = bytes_per_pixel(config.format);
  if (!config.address || !config.width || !config.height || !bpp)
    return false;
  if (config.width > config.pitch / bpp)
    return false;
  fb = config;
  initialized = true;
  return true;
}

bool valid() { return initialized; }
uint32_t width() { return initialized ? fb.width : 0; }
uint32_t height() { return initialized ? fb.height : 0; }
FBInfo get_info() {
  return {fb.address, fb.width, fb.height, fb.pitch, fb.format, initialized};
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  if (!initialized || x >= fb.width || y >= fb.height) [[unlikely]]
    return;

  volatile uint8_t *dst = static_cast<volatile uint8_t *>(fb.address) +
                          static_cast<size_t>(y) * fb.pitch +
                          static_cast<size_t>(x) * bytes_per_pixel(fb.format);

  const uint8_t r = (color >> 16) & 0xFF;
  const uint8_t g = (color >> 8) & 0xFF;
  const uint8_t b = color & 0xFF;

  switch (fb.format) {
  case Format::RGB24:
    dst[0] = r;
    dst[1] = g;
    dst[2] = b;
    break;
  case Format::BGR24:
    dst[0] = b;
    dst[1] = g;
    dst[2] = r;
    break;
  case Format::RGB32:
    dst[0] = r;
    dst[1] = g;
    dst[2] = b;
    dst[3] = 0;
    break;
  case Format::BGR32:
    dst[0] = b;
    dst[1] = g;
    dst[2] = r;
    dst[3] = 0;
    break;
  case Format::RGB565:
  case Format::BGR565: {
    uint16_t pixel = (fb.format == Format::RGB565)
                         ? ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                         : ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3);
    dst[0] = pixel & 0xFF;
    dst[1] = pixel >> 8;
    break;
  }
  }
}

void clear(uint32_t color) { fill(color); }

void fill(uint32_t color) {
  if (!initialized)
    return;
  fill_rect(0, 0, fb.width, fb.height, color);
}

void fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
  if (!initialized)
    return;
  const int32_t sw = static_cast<int32_t>(fb.width);
  const int32_t sh = static_cast<int32_t>(fb.height);
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > sw)
    w = sw - x;
  if (y + h > sh)
    h = sh - y;
  if (w <= 0 || h <= 0)
    return;

  const uint32_t ux = static_cast<uint32_t>(x);
  const uint32_t uy = static_cast<uint32_t>(y);
  const uint32_t uw = static_cast<uint32_t>(w);
  const uint32_t uh = static_cast<uint32_t>(h);

  auto *base = static_cast<volatile uint8_t *>(fb.address);
  const size_t pitch = fb.pitch;
  const uint8_t r = static_cast<uint8_t>(color >> 16);
  const uint8_t g = static_cast<uint8_t>(color >> 8);
  const uint8_t b = static_cast<uint8_t>(color);

  switch (fb.format) {
  case Format::RGB32:
  case Format::BGR32: {
    const uint32_t pixel =
        (fb.format == Format::RGB32)
            ? (uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16))
            : (uint32_t(b) | (uint32_t(g) << 8) | (uint32_t(r) << 16));

    for (uint32_t py = 0; py < uh; ++py) {
      auto *dst = reinterpret_cast<volatile uint32_t *>(
          base + size_t(uy + py) * pitch + size_t(ux) * 4);
      uint32_t px = 0;
      for (; px + 4 <= uw; px += 4) {
        dst[px + 0] = pixel;
        dst[px + 1] = pixel;
        dst[px + 2] = pixel;
        dst[px + 3] = pixel;
      }
      for (; px < uw; ++px)
        dst[px] = pixel;
    }
    break;
  }
  case Format::RGB565:
  case Format::BGR565: {
    const uint16_t pixel =
        (fb.format == Format::RGB565)
            ? uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
            : uint16_t(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));

    for (uint32_t py = 0; py < uh; ++py) {
      auto *dst = reinterpret_cast<volatile uint16_t *>(
          base + size_t(uy + py) * pitch + size_t(ux) * 2);
      uint32_t px = 0;
      for (; px + 4 <= uw; px += 4) {
        dst[px + 0] = pixel;
        dst[px + 1] = pixel;
        dst[px + 2] = pixel;
        dst[px + 3] = pixel;
      }
      for (; px < uw; ++px)
        dst[px] = pixel;
    }
    break;
  }
  case Format::RGB24:
  case Format::BGR24: {
    const bool rgb = (fb.format == Format::RGB24);
    const uint8_t c0 = rgb ? r : b;
    const uint8_t c1 = g;
    const uint8_t c2 = rgb ? b : r;

    for (uint32_t py = 0; py < uh; ++py) {
      auto *row = base + size_t(uy + py) * pitch + size_t(ux) * 3;
      for (uint32_t px = 0; px < uw; ++px) {
        row[px * 3 + 0] = c0;
        row[px * 3 + 1] = c1;
        row[px * 3 + 2] = c2;
      }
    }
    break;
  }
  }
}

void blit(const uint32_t *src, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
          uint32_t src_pitch) {
  if (!initialized || !src || x >= fb.width || y >= fb.height) [[unlikely]]
    return;
  w = std::min(w, fb.width - x);
  h = std::min(h, fb.height - y);
  if (w == 0 || h == 0)
    return;

  const uint8_t bpp = bytes_per_pixel(fb.format);
  auto *dst_base = reinterpret_cast<uint8_t *>(
      const_cast<uint8_t *>(static_cast<const volatile uint8_t *>(fb.address)));
  size_t dst_offset =
      static_cast<size_t>(y) * fb.pitch + static_cast<size_t>(x) * bpp;
  const uint8_t *src_curr = reinterpret_cast<const uint8_t *>(src);

  for (uint32_t row = 0; row < h; ++row) {
    auto *d = dst_base + dst_offset;
    const auto *s = reinterpret_cast<const uint32_t *>(src_curr);

    switch (fb.format) {
    case Format::BGR32:
      copy_bytes(d, src_curr, static_cast<size_t>(w) * sizeof(uint32_t));
      break;
    case Format::RGB32:
      for (uint32_t px = 0; px < w; ++px)
        reinterpret_cast<uint32_t *>(d)[px] = swap_red_blue(s[px]);
      break;
    case Format::RGB24:
    case Format::BGR24: {
      const bool rgb = fb.format == Format::RGB24;
      for (uint32_t px = 0; px < w; ++px) {
        const uint32_t color = s[px];
        uint8_t *pixel = d + static_cast<size_t>(px) * 3;
        const uint8_t r = static_cast<uint8_t>(color >> 16);
        const uint8_t g = static_cast<uint8_t>(color >> 8);
        const uint8_t b = static_cast<uint8_t>(color);
        pixel[0] = rgb ? r : b;
        pixel[1] = g;
        pixel[2] = rgb ? b : r;
      }
      break;
    }
    case Format::RGB565:
    case Format::BGR565: {
      const bool rgb = fb.format == Format::RGB565;
      auto *pixels = reinterpret_cast<uint16_t *>(d);
      for (uint32_t px = 0; px < w; ++px) {
        const uint32_t color = s[px];
        const uint8_t r = static_cast<uint8_t>(color >> 16);
        const uint8_t g = static_cast<uint8_t>(color >> 8);
        const uint8_t b = static_cast<uint8_t>(color);
        pixels[px] =
            rgb ? uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
                : uint16_t(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
      }
      break;
    }
    }
    dst_offset += fb.pitch;
    src_curr += src_pitch;
  }
}

} // namespace Kernel::Drivers::Framebuffer
