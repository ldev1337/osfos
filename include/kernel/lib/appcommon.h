#pragma once

#include <stdint.h>
#include <stddef.h>
#include "kernel/graphics/vfb.h"

namespace Kernel::AppCommon {

void text(Kernel::Graphics::VirtualFramebuffer& vfb, int32_t x, int32_t y, const char* s, uint32_t color);
void number(Kernel::Graphics::VirtualFramebuffer& vfb, int32_t x, int32_t y, int32_t n, uint32_t color);
void delay(uint32_t speed);
bool request_window_size(uint32_t requested_width, uint32_t requested_height,
                         uint32_t& actual_width, uint32_t& actual_height);

}
