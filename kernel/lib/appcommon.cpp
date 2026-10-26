#include <kernel/lib/appcommon.h>
#include <kernel/drivers/pit.h>
#include <kernel/drivers/keyboard.h>
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
    const uint32_t target_ticks = 1000 / (20 * speed); 
    
    static uint32_t last_tick = 0;
    uint32_t current_tick = Kernel::Drivers::PIT::get_ticks();
    
    while ((current_tick - last_tick) < target_ticks) {
        asm volatile("pause");
        current_tick = Kernel::Drivers::PIT::get_ticks();
    }
    
    last_tick = current_tick;
}

} 