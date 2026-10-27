#include <kernel/lib/appcommon.h>
#include <kernel/drivers/pit.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/scheduler.h>
#include <kernel/graphics/window_manager.h>
#include <arch/x86_64/string.h>

namespace Kernel::AppCommon {

void text(Kernel::Graphics::VirtualFramebuffer& vfb, int32_t x, int32_t y, const char* s, uint32_t color) {
    while (*s) {
        vfb.draw_glyph(x, y, *s++, color);
        x += 8;
    }
}

void number(Kernel::Graphics::VirtualFramebuffer& vfb, int32_t x, int32_t y, int32_t n, uint32_t color) {
    vfb.draw_glyph(x, y, static_cast<char>('0' + (n / 10) % 10), color);
    vfb.draw_glyph(x + 8, y, static_cast<char>('0' + n % 10), color);
}

void delay(uint32_t speed) {
    if (!speed) speed = 1;
    const uint32_t target_ticks = (1000 / (60 * speed)) ? (1000 / (60 * speed)) : 1;
    Kernel::Scheduler::thread_sleep_ms(target_ticks);
}

bool request_window_size(uint32_t requested_width, uint32_t requested_height,
                         uint32_t& actual_width, uint32_t& actual_height) {
    return Kernel::Graphics::WindowManager::request_window_size(
        Kernel::Scheduler::current_thread_id(), requested_width, requested_height,
        &actual_width, &actual_height);
}

}
