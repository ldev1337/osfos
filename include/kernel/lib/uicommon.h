#pragma once

#include "kernel/graphics/vfb.h"
#include <cstddef>
#include <cstdint>

namespace Kernel::UICommon {

void draw_button(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x,
                 int32_t y, int32_t w, int32_t h, const char *label,
                 uint32_t bg_color, uint32_t text_color, uint32_t border_color);
void draw_status_bar(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x,
                     int32_t y, int32_t w, int32_t h, const char *status_text,
                     uint32_t bg_color, uint32_t text_color,
                     uint32_t border_color);
bool prompt_filename(Kernel::Graphics::VirtualFramebuffer &vfb,
                     const char *prompt, char *buf, size_t max_len);

} // namespace Kernel::UICommon