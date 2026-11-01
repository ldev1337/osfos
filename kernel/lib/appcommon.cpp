#include <kernel/lib/appcommon.h>
#include <kernel/drivers/pit.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
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

bool get_window_position(uint64_t thread_id, int* x, int* y) {
    return Kernel::Graphics::WindowManager::get_window_position(thread_id, x, y);
}

static int mouse_position(bool horizontal) {
    int wx = 0, wy = 0;
    int position = horizontal ? Kernel::Drivers::Mouse::get_x() : Kernel::Drivers::Mouse::get_y();
    uint64_t tid = Kernel::Scheduler::current_thread_id();
    return get_window_position(tid, &wx, &wy) ? position - (horizontal ? wx : wy) : position;
}

int get_mouse_x() { return mouse_position(true); }
int get_mouse_y() { return mouse_position(false); }

bool is_left_pressed() {
    return Kernel::Drivers::Mouse::is_left_pressed();
}

}