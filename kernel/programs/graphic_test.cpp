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

constexpr uint32_t BG = 0x1B1111;
constexpr uint32_t FG = 0xF4D6CD;
constexpr uint32_t BLUE = 0xFAB489;
constexpr uint32_t GREEN = 0xA1E3A6;
constexpr uint32_t RED = 0xA88BF3;
constexpr uint32_t YELLOW = 0xAFE2F9;
constexpr uint32_t ORANGE = 0xF3B089;
constexpr uint32_t PURPLE = 0xD489FA;

void run_graphic_test() {
    if (!Kernel::Drivers::Framebuffer::valid()) return;

    int32_t w = static_cast<int32_t>(width());
    int32_t h = static_cast<int32_t>(height());
    if (w < 200 || h < 150) return;

    Kernel::Graphics::VirtualFramebuffer vfb(0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    if (!vfb.valid()) return;

    int page = 0;
    int max_pages = 4;
    bool running = true;
    uint32_t anim_counter = 0;

    while (running) {
        while (Keyboard::available()) {
            char c = Keyboard::getchar();
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c == 'q' || c == 27) {
                running = false;
                break;
            }
            if (c == ' ' || c == '\n' || c == '\r') {
                page = (page + 1) % max_pages;
            }
        }
        if (!running) break;

        vfb.clear(BG);

        
        text(vfb, 20, 15, "GRAPHIC TEST SUITE - DIVERSE MODULES", BLUE);
        text(vfb, 20, h - 25, "SPACE: NEXT PAGE    Q: QUIT", FG);
        number(vfb, w - 60, 15, page + 1, GREEN);
        text(vfb, w - 40, 15, "/4", FG);

        if (page == 0) {
            text(vfb, 20, 45, "1. Basic Lines, Rectangles & Triangles", YELLOW);
            
            vfb.draw_line(50, 80, 160, 130, GREEN);
            vfb.draw_line(160, 80, 50, 130, RED);

            vfb.fill_rect(190, 75, 90, 55, BLUE);
            vfb.fill_rect_alpha(210, 95, 90, 55, GREEN, 140);

            vfb.draw_circle(360, 100, 32, YELLOW);
            vfb.fill_circle(450, 100, 28, RED);

            vfb.draw_triangle(520, 130, 570, 70, 620, 130, BLUE);
            vfb.fill_triangle(650, 130, 700, 70, 750, 130, PURPLE);

            int32_t cx = 100 + static_cast<int32_t>(anim_counter % static_cast<uint32_t>(w - 200));
            vfb.fill_circle_alpha(cx, 210, 22, ORANGE, 200);
            text(vfb, 20, 250, "Dynamic element traversal and alpha layers active.", FG);

        } else if (page == 1) {
            text(vfb, 20, 45, "2. Angular Geometry, Arcs & Sectors", YELLOW);

            int32_t xc = 110, yc = 120, r = 45;
            vfb.draw_arc(xc, yc, r, 0, 270, GREEN);
            text(vfb, 70, 180, "Arc", FG);

            int32_t xc2 = 250, yc2 = 120;
            int32_t dynamic_angle = static_cast<int32_t>((anim_counter * 4) % 360);
            vfb.draw_semicircle(xc2, yc2, r, dynamic_angle, BLUE);
            text(vfb, 200, 180, "Semicircle", FG);

            int32_t xc3 = 390, yc3 = 120;
            vfb.fill_sector(xc3, yc3, r, 15, 150, RED);
            text(vfb, 350, 180, "Sector", FG);

            int32_t xc4 = 530, yc4 = 120;
            vfb.fill_sector(xc4, yc4, r, 180, 310, PURPLE);
            text(vfb, 480, 180, "Pie Wedge", FG);

            int32_t xc5 = 670, yc5 = 120;
            vfb.fill_circle_alpha(xc5, yc5, r, YELLOW, 110);
            text(vfb, 610, 180, "Alpha Ring", FG);

        } else if (page == 2) {
            text(vfb, 20, 45, "3. Rounded Rectangles & UI Paneling", YELLOW);

            vfb.draw_rounded_rect(50, 80, 150, 65, 10, GREEN);
            text(vfb, 65, 105, "Outline Widget", FG);

            vfb.fill_rounded_rect(220, 80, 150, 65, 14, BLUE);
            text(vfb, 245, 105, "Solid Button", BG);

            vfb.fill_rounded_rect(390, 80, 180, 65, 20, RED);
            text(vfb, 420, 105, "Container Panel", FG);

            vfb.draw_rounded_rect(590, 80, 160, 65, 25, PURPLE);
            text(vfb, 615, 105, "Pill Badge", YELLOW);

            text(vfb, 20, 190, "Advanced UI bounding boxes and corner radii rendered cleanly.", GREEN);

        } else if (page == 3) {
            text(vfb, 20, 45, "4. Multi-Layer Blending & Matrix Composition", YELLOW);

            for (int i = 0; i < 6; ++i) {
                int32_t px = 100 + i * 95;
                int32_t py = 110 + (i % 2) * 25;
                uint32_t col = (i % 2 == 0) ? GREEN : ORANGE;
                uint8_t alpha = static_cast<uint8_t>(60 + i * 30);
                vfb.fill_circle_alpha(px, py, 35, col, alpha);
                vfb.fill_rounded_rect(px - 20, py - 10, 40, 20, 6, PURPLE);
            }

            text(vfb, 20, 210, "Stress testing composition blending rules across layers.", FG);
        }

        vfb.flush();
        anim_counter++;
        delay(2);
    }
}
} 

void graphic_test_run() {
    run_graphic_test();
}
} 