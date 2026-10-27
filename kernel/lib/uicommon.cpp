#include <kernel/lib/uicommon.h>
#include <kernel/lib/appcommon.h>
#include <kernel/drivers/keyboard.h>
#include <arch/x86_64/string.h>

namespace Kernel::UICommon {

void draw_button(Kernel::Graphics::VirtualFramebuffer& vfb, int32_t x, int32_t y, int32_t w, int32_t h, const char* label, uint32_t bg_color, uint32_t text_color, uint32_t border_color) {
    vfb.fill_rect(x, y, w, h, bg_color);
    if (border_color != 0) {
        vfb.draw_rect(x, y, w, h, border_color);
    }
    Kernel::AppCommon::text(vfb, x + 8, y + (h / 2) - 4, label, text_color);
}

void draw_status_bar(Kernel::Graphics::VirtualFramebuffer& vfb, int32_t x, int32_t y, int32_t w, int32_t h, const char* status_text, uint32_t bg_color, uint32_t text_color, uint32_t border_color) {
    vfb.fill_rect(x, y, w, h, bg_color);
    if (border_color != 0) {
        vfb.draw_line(x, y, x + w, y, border_color);
    }
    Kernel::AppCommon::text(vfb, x + 10, y + (h / 2) - 4, status_text, text_color);
}

bool prompt_filename(Kernel::Graphics::VirtualFramebuffer& vfb, const char* prompt, char* buf, size_t max_len) {
    size_t len = strlen(buf);
    while (true) {
        vfb.fill_rect(20, 80, 320, 70, 0xFF11111B);
        vfb.draw_rect(20, 80, 320, 70, 0xFF89B4FA);
        Kernel::AppCommon::text(vfb, 30, 90, prompt, 0xFFCDD6F4);
        Kernel::AppCommon::text(vfb, 30, 115, buf, 0xFFF9E2AF);
        vfb.flush();

        char c = Kernel::Drivers::Keyboard::getchar();
        if (c == '\n') return true;
        if (c == 27) return false;
        if (c == '\b' && len > 0) {
            buf[--len] = '\0';
        } else if (c >= 32 && c < 127 && len + 1 < max_len) {
            buf[len++] = c;
            buf[len] = '\0';
        }
    }
}

}