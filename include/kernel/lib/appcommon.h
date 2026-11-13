#pragma once

#include "kernel/graphics/vfb.h"
#include <cstdint>

namespace Kernel::AppCommon {

void text(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x, int32_t y,
          const char *s, uint32_t color, float font_size_pixels = 14.0f);
int text_width(const char *s, float font_size_pixels = 14.0f);
void number(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x, int32_t y,
            int32_t n, uint32_t color);
void delay(uint32_t speed);
[[noreturn]] void exit(int status = 0);
bool get_window_position(uint64_t thread_id, int *x, int *y);
int get_mouse_x();
int get_mouse_y();
bool is_left_pressed();
} // namespace Kernel::AppCommon
