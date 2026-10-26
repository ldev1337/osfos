#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/appcommon.h>
#include <stdint.h>

namespace Kernel::Apps {
namespace {
using Kernel::Drivers::Framebuffer::width;
using Kernel::Drivers::Framebuffer::height;
using namespace Kernel::AppCommon;
using Kernel::Drivers::Keyboard;

constexpr uint32_t BG = 0x11111b;
constexpr uint32_t FG = 0xcdd6f4;
constexpr uint32_t BLUE = 0x89b4fa;
constexpr uint32_t GREEN = 0xa6e3a1;
constexpr uint32_t RED = 0xf38ba8;
constexpr uint32_t YELLOW = 0xf9e2af;

void run_stress_test() {
    if (!Kernel::Drivers::Framebuffer::valid()) return;

    int32_t w = static_cast<int32_t>(width());
    int32_t h = static_cast<int32_t>(height());
    if (w < 200 || h < 150) return;

    Kernel::Graphics::VirtualFramebuffer vfb(0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    if (!vfb.valid()) return;

    int mode = 0;
    bool running = true;
    uint32_t frame_count = 0;

    while (running) {
        while (Keyboard::available()) {
            char c = Keyboard::getchar();
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c == 'q' || c == 27) {
                running = false;
                break;
            }
            if (c == ' ' || c == '\n' || c == '\r') {
                mode = (mode + 1) % 3;
            }
        }
        if (!running) break;

        vfb.clear(BG);

        text(vfb, 20, 15, "KERNEL STRESS TEST BENCHMARK", BLUE);
        text(vfb, 20, h - 25, "SPACE: SWITCH MODE    Q: QUIT", FG);
        text(vfb, w - 140, 15, "MODE:", YELLOW);
        number(vfb, w - 80, 15, mode + 1, GREEN);

        if (mode == 0) {
            text(vfb, 20, 45, "Test 1: Heavy Primitive Flood (Rectangles & Rounded Shapes)", YELLOW);
            for (int i = 0; i < 150; ++i) {
                int32_t px = (i * 37 + frame_count * 5) % (w - 50);
                int32_t py = 60 + (i * 23 + frame_count * 3) % (h - 120);
                uint32_t col = (i % 2 == 0) ? GREEN : RED;
                vfb.fill_rounded_rect(px, py, 30, 20, 4, col);
            }
        } else if (mode == 1) {
            text(vfb, 20, 45, "Test 2: Heavy Alpha Blending Overload", YELLOW);
            for (int i = 0; i < 80; ++i) {
                int32_t cx = 50 + (i * 41 + frame_count * 7) % (w - 100);
                int32_t cy = 80 + (i * 29 + frame_count * 4) % (h - 140);
                uint32_t col = (i % 3 == 0) ? BLUE : (i % 3 == 1) ? YELLOW : RED;
                uint8_t alpha = static_cast<uint8_t>(50 + (i * 15) % 150);
                vfb.fill_circle_alpha(cx, cy, 35, col, alpha);
            }
        } else if (mode == 2) {
            text(vfb, 20, 45, "Test 3: Line Matrix Rasterization Stress", YELLOW);
            int32_t ox = w / 2;
            int32_t oy = h / 2;
            for (int i = 0; i < 60; ++i) {
                int32_t angle_offset = static_cast<int32_t>((frame_count + i * 6) % 360);
                int32_t x2 = ox + (150 * (i % 2 == 1 ? 1 : -1) + angle_offset) % (w / 2);
                int32_t y2 = oy + (100 * (i % 3 == 1 ? 1 : -1)) % (h / 2);
                uint32_t col = (i % 2 == 0) ? GREEN : BLUE;
                vfb.draw_line(ox, oy, x2, y2, col);
            }
        }

        vfb.flush();
        frame_count++;
        delay(1);
    }
}
} 

void stress_test_run() {
    run_stress_test();
}
} 