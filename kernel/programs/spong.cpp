#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/lib/appcommon.h>
#include <kernel/lib/random.h>
#include <kernel/graphics/vfb.h>
#include <stdint.h>

namespace Kernel::Games::Spong {
namespace {
using Kernel::Drivers::Framebuffer::width;
using Kernel::Drivers::Framebuffer::height;
using Kernel::Drivers::Keyboard;
using namespace Kernel::AppCommon;

constexpr uint32_t BG = 0x1B1111;
constexpr uint32_t FG = 0xF4D6CD;
constexpr uint32_t BLUE = 0xFAB489;
constexpr uint32_t GREEN = 0xA1E3A6;
constexpr uint32_t RED = 0xA88BF3;

constexpr int32_t PW = 10, PH = 76, BW = 10;


constexpr uint8_t KEY_Q = 0x10;
constexpr uint8_t KEY_W = 0x11;
constexpr uint8_t KEY_S = 0x1F;
constexpr uint8_t KEY_I = 0x17;
constexpr uint8_t KEY_K = 0x25;

struct Game {
    int32_t w, h, p1, p2, bx, by, vx, vy;
    int32_t score1, score2, speed;
    int32_t ai_target;
    int32_t ai_timer;
    bool ai;
    Kernel::Lib::Random rng{};
};

void reset_ball(Game& g, int32_t direction) {
    g.bx = g.w / 2;
    g.by = g.h / 2;
    g.vx = direction * (4 + g.speed);
    g.vy = static_cast<int32_t>(g.rng.next_range(2, 4 + g.speed));
    if (g.rng.next_range(0, 1)) g.vy = -g.vy;

    g.ai_target = g.h / 2 - PH / 2;
    g.ai_timer = 0;
}

void render(const Game& g, Kernel::Graphics::VirtualFramebuffer& vfb) {
    vfb.clear(BG);

    for (int32_t y = 0; y < g.h; y += 24)
        vfb.fill_rect(g.w / 2 - 1, y, 2, 12, 0x2E1E1E);

    vfb.fill_rect(20, g.p1, PW, PH, GREEN);
    vfb.fill_rect(g.w - 20 - PW, g.p2, PW, PH, RED);
    vfb.fill_rect(g.bx, g.by, BW, BW, FG);

    text(vfb, g.w / 2 - 80, 20, "SPONG", BLUE);
    number(vfb, g.w / 2 - 24, 20, g.score1, GREEN);
    number(vfb, g.w / 2 + 8, 20, g.score2, RED);
    text(vfb, 16, g.h - 20, g.ai ? "W/S   AI   Q: QUIT"
                                 : "W/S   I/K   Q: QUIT", FG);
    vfb.flush();
}

void update_ai(Game& g) {
    if (!g.ai) return;
    if (++g.ai_timer > 10) {
        g.ai_timer = 0;
        int32_t raw_target = g.by + BW / 2 - PH / 2;
        int32_t error_margin = static_cast<int32_t>(g.rng.next_range(-22, 22));
        g.ai_target = raw_target + error_margin;
    }

    if (g.ai_target < 0) g.ai_target = 0;
    if (g.ai_target > g.h - PH) g.ai_target = g.h - PH;

    int32_t delta = g.ai_target - g.p2;
    int32_t tracking_speed = 2 + (g.speed > 1 ? 1 : 0);

    if (delta > tracking_speed) {
        g.p2 += tracking_speed;
    } else if (delta < -tracking_speed) {
        g.p2 -= tracking_speed;
    } else {
        g.p2 = g.ai_target;
    }
}

void input(Game& g, bool& running) {
    if (Keyboard::is_pressed(KEY_Q)) {
        running = false;
        return;
    }

    int32_t paddle_speed = 6;
    if (Keyboard::is_pressed(KEY_W)) g.p1 -= paddle_speed;
    if (Keyboard::is_pressed(KEY_S)) g.p1 += paddle_speed;

    if (!g.ai) {
        if (Keyboard::is_pressed(KEY_I)) g.p2 -= paddle_speed;
        if (Keyboard::is_pressed(KEY_K)) g.p2 += paddle_speed;
    } else {
        update_ai(g);
    }

    if (g.p1 < 0) g.p1 = 0;
    if (g.p2 < 0) g.p2 = 0;
    if (g.p1 > g.h - PH) g.p1 = g.h - PH;
    if (g.p2 > g.h - PH) g.p2 = g.h - PH;
}
} 

void run(uint32_t speed, bool ai) {
    Game g{};
    g.w = static_cast<int32_t>(width());
    g.h = static_cast<int32_t>(height());
    if (g.w < 120 || g.h < 100) return;

    Kernel::Graphics::VirtualFramebuffer vfb(0, 0, static_cast<uint32_t>(g.w), static_cast<uint32_t>(g.h));
    if (!vfb.valid()) return;

    if (speed < 1) speed = 1;
    if (speed > 5) speed = 5;

    g.speed = static_cast<int32_t>(speed);
    g.ai = ai;
    g.p1 = g.p2 = (g.h - PH) / 2;
    g.score1 = g.score2 = 0;
    g.rng = Kernel::Lib::Random(
        Kernel::Lib::Random::rdtsc() ^
        (static_cast<uint64_t>(g.w) << 32) ^
        static_cast<uint64_t>(g.h));

    reset_ball(g, 1);

    bool running = true;
    while (running) {
        input(g, running);
        if (!running) break;

        g.bx += g.vx;
        g.by += g.vy;

        if (g.by <= 0) {
            g.by = 0;
            g.vy = -g.vy;
        } else if (g.by + BW >= g.h) {
            g.by = g.h - BW;
            g.vy = -g.vy;
        }

        if (g.vx < 0 && g.bx <= 20 + PW &&
            g.bx + BW >= 20 && g.by + BW >= g.p1 &&
            g.by <= g.p1 + PH) {
            g.bx = 20 + PW;
            g.vx = -g.vx;
        }

        if (g.vx > 0 && g.bx + BW >= g.w - 20 - PW &&
            g.bx <= g.w - 20 && g.by + BW >= g.p2 &&
            g.by <= g.p2 + PH) {
            g.bx = g.w - 20 - PW - BW;
            g.vx = -g.vx;
        }

        if (g.bx < 0) {
            ++g.score2;
            reset_ball(g, 1);
        } else if (g.bx + BW >= g.w) {
            ++g.score1;
            reset_ball(g, -1);
        }

        render(g, vfb);
        delay(g.speed);
    }
}
} 