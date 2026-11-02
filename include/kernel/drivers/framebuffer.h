#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Kernel::Drivers::Framebuffer {

enum class Format : uint8_t { RGB24, BGR24, RGB32, BGR32, RGB565, BGR565 };

struct Config {
  volatile void *address;
  uint32_t width, height, pitch;
  Format format;
};

struct FBInfo {
  volatile void *address;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  Format format;
  bool initialized;
};

bool init(const Config &config);
bool valid();
uint32_t width();
uint32_t height();
FBInfo get_info();

void put_pixel(uint32_t x, uint32_t y, uint32_t color);
void clear(uint32_t color);
void fill(uint32_t color);
void fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void blit(const uint32_t *src, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
          uint32_t src_pitch);

} // namespace Kernel::Drivers::Framebuffer