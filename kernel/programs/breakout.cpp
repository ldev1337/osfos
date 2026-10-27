#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/lib/appcommon.h>
#include <kernel/scheduler.h>
#include <kernel/graphics/vfb.h>
#include <kernel/graphics/window_manager.h>
#include <stdint.h>

namespace Kernel::Games::Breakout {
namespace {
using Kernel::Drivers::Framebuffer::width;
using Kernel::Drivers::Framebuffer::height;
using Kernel::Drivers::Keyboard;
static Kernel::Graphics::VirtualFramebuffer vfb;

constexpr uint32_t BG    = 0x1B1111;
constexpr uint32_t PANEL   = 0x2E1E1E;
constexpr uint32_t FG      = 0xF4D6CD;
constexpr uint32_t MUTED   = 0xC8ADA6;
constexpr uint32_t BLUE    = 0xFAB489;
constexpr uint32_t GREEN   = 0xA1E3A6;
constexpr uint32_t RED     = 0xA88BF3;
constexpr uint32_t YELLOW  = 0xAFE2F9;

constexpr int COLS = 10;
constexpr int BRICK_ROWS = 7;
constexpr int MAX_BRICKS = COLS * BRICK_ROWS;
constexpr int BALL_R = 7;
constexpr int PADDLE_H = 10;
constexpr int BRICK_H = 16;
constexpr int GAP = 4;

constexpr uint8_t KEY_Q = 0x10;
constexpr uint8_t KEY_A = 0x1E;
constexpr uint8_t KEY_D = 0x20;
constexpr uint8_t KEY_J = 0x24;
constexpr uint8_t KEY_L = 0x26;
constexpr uint8_t KEY_SPACE = 0x39;

static bool bricks[MAX_BRICKS];
static int sw, sh, paddle_w, paddle_x, paddle_y;
constexpr int MAX_BALLS = 8;
constexpr int MAX_POWERUPS = 12;
constexpr int POWERUP_SIZE = 10;
constexpr int POWERUP_SPEED = 2;

struct Ball {
    int x, y, vx, vy;
    bool active;
};

struct PowerUp {
    int x, y;
    bool active;
};

static Ball balls[MAX_BALLS];
static PowerUp powerups[MAX_POWERUPS];
static int score, lives, remaining;
static bool launched, finished, won;

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int brick_w() {
    return (sw - 32 - (COLS - 1) * GAP) / COLS;
}

static int brick_x(int c) {
    return 16 + c * (brick_w() + GAP);
}

static int brick_y(int r) {
    return 54 + r * (BRICK_H + GAP);
}

static void attach_ball() {
    for (int i = 0; i < MAX_BALLS; ++i)
        balls[i].active = false;

    balls[0] = {
        paddle_x + paddle_w / 2,
        paddle_y - BALL_R - 1,
        2, -3, true
    };
    launched = false;
}

static void reset_game() {
    for (int i = 0; i < MAX_BRICKS; ++i) bricks[i] = true;
    score = 0;
    lives = 3;
    remaining = MAX_BRICKS;
    finished = won = false;
    paddle_w = clamp(sw / 7, 64, 110);
    paddle_x = (sw - paddle_w) / 2;
    paddle_y = sh - 42;
    attach_ball();
    for (int i = 0; i < MAX_POWERUPS; ++i) powerups[i].active = false;
}

static void text_center(int y, const char* s, uint32_t color) {
    int n = 0;
    for (const char* p = s; *p; ++p) ++n;
    int x = (sw - n * 8) / 2;
    if (x < 0) x = 0;
    Kernel::AppCommon::text(vfb, x, y, s, color);
}

static bool overlaps_rect(
    int cx, int cy, int radius,
    int rx, int ry, int rw, int rh
) {
    int nx = clamp(cx, rx, rx + rw);
    int ny = clamp(cy, ry, ry + rh);
    int dx = cx - nx;
    int dy = cy - ny;
    return dx * dx + dy * dy <= radius * radius;
}

static void spawn_duplicate_ball(int x, int y) {
    for (int i = 0; i < MAX_BALLS; ++i) {
        if (balls[i].active) continue;

        balls[i] = {x, y, -2, -3, true};
        return;
    }
}

static void spawn_powerup(int x, int y) {
    for (int i = 0; i < MAX_POWERUPS; ++i) {
        if (powerups[i].active) continue;
        powerups[i] = {x, y, true};
        return;
    }
}

static bool hit_brick(Ball& b, bool horizontal) {
    for (int r = 0; r < BRICK_ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            const int i = r * COLS + c;
            if (!bricks[i]) continue;

            const int bx = brick_x(c);
            const int by = brick_y(r);
            const int bw = brick_w();

            if (!overlaps_rect(
                    b.x, b.y, BALL_R,
                    bx, by, bw, BRICK_H))
                continue;

            bricks[i] = false;
            --remaining;
            score += (BRICK_ROWS - r) * 10;

            if (horizontal)
                b.vx = -b.vx;
            else
                b.vy = -b.vy;

            if (remaining > 0 && (score / 10 + i) % 5 == 0)
                spawn_powerup(bx + bw / 2, by + BRICK_H);

            return true;
        }
    }

    return false;
}

static void draw() {
    vfb.clear(BG);

    vfb.fill_rect(0, 0, sw, 34, PANEL);
    vfb.fill_rect(0, 33, sw, 2, BLUE);
    Kernel::AppCommon::text(vfb, 12, 12, "OSFOS / BREAKOUT", GREEN);
    Kernel::AppCommon::text(vfb, sw / 2 - 56, 12, "SCORE", MUTED);
    Kernel::AppCommon::number(vfb, sw / 2 + 4, 12, score, FG);
    Kernel::AppCommon::text(vfb, sw - 100, 12, "LIVES", MUTED);
    Kernel::AppCommon::number(vfb, sw - 48, 12, lives, FG);

    for (int r = 0; r < BRICK_ROWS; ++r) {
        const uint32_t colors[] = {RED, YELLOW, GREEN, BLUE, RED};
        for (int c = 0; c < COLS; ++c) {
            int i = r * COLS + c;
            if (!bricks[i]) continue;
            int x = brick_x(c), y = brick_y(r);
            vfb.fill_rect(x, y, brick_w(), BRICK_H, colors[r]);
        }
    }

    vfb.fill_rect(paddle_x, paddle_y, paddle_w, PADDLE_H, BLUE);
    for (int i = 0; i < MAX_BALLS; ++i) {
        if (balls[i].active)
            vfb.fill_circle(balls[i].x, balls[i].y, BALL_R, FG);
    }

    for (int i = 0; i < MAX_POWERUPS; ++i) {
        if (!powerups[i].active) continue;

        int x = powerups[i].x;
        int y = powerups[i].y;
        vfb.fill_rounded_rect(
            x - POWERUP_SIZE / 2,
            y - POWERUP_SIZE / 2,
            POWERUP_SIZE, POWERUP_SIZE, 3, YELLOW);
    }

    if (!launched && !finished)
        text_center(sh / 2 + 35, "SPACE TO LAUNCH", FG);

    if (finished) {
        vfb.fill_rect(sw / 2 - 150, sh / 2 - 48, 300, 100, PANEL);
        vfb.draw_rounded_rect(sw / 2 - 150, sh / 2 - 48, 300, 100, 8, BLUE);
        text_center(sh / 2 - 24, won ? "YOU WIN!" : "GAME OVER", won ? GREEN : RED);
        text_center(sh / 2 + 2, "SPACE: RESTART", FG);
        text_center(sh / 2 + 22, "Q: QUIT", MUTED);
    } else {
        Kernel::AppCommon::text(vfb, 12, sh - 20, "A/D or J/L: MOVE    SPACE: LAUNCH    Q: QUIT", MUTED);
    }

    vfb.flush();
}

static void update() {
    if (!launched) {
        balls[0].x = paddle_x + paddle_w / 2;
        balls[0].y = paddle_y - BALL_R - 1;
    }

    if (launched) {
        for (int n = 0; n < MAX_BALLS; ++n) {
            Ball& b = balls[n];
            if (!b.active) continue;

            int sx = b.vx < 0 ? -1 : 1;
            for (int k = 0; k < (b.vx < 0 ? -b.vx : b.vx); ++k) {
                int old_x = b.x;
                b.x += sx;

                if (b.x - BALL_R < 0 || b.x + BALL_R >= sw) {
                    b.x = old_x;
                    b.vx = -b.vx;
                    break;
                }

                if (hit_brick(b, true)) {
                    b.x = old_x;
                    break;
                }
            }

            int sy = b.vy < 0 ? -1 : 1;
            for (int k = 0; k < (b.vy < 0 ? -b.vy : b.vy); ++k) {
                int old_y = b.y;
                b.y += sy;

                if (b.y - BALL_R < 36) {
                    b.y = old_y;
                    b.vy = -b.vy;
                    break;
                }

                if (b.vy > 0 &&
                    old_y + BALL_R <= paddle_y &&
                    b.y + BALL_R >= paddle_y &&
                    b.x + BALL_R >= paddle_x &&
                    b.x - BALL_R <= paddle_x + paddle_w) {

                    b.y = paddle_y - BALL_R - 1;
                    b.vy = - (b.vy < 0 ? -b.vy : b.vy);

                    int hit = b.x - (paddle_x + paddle_w / 2);
                    b.vx = clamp(hit / 10, -4, 4);
                    if (b.vx == 0)
                        b.vx = hit < 0 ? -1 : 1;

                    break;
                }

                if (hit_brick(b, false)) {
                    b.y = old_y;
                    break;
                }
            }

            if (b.y - BALL_R >= sh)
                b.active = false;
        }

        bool any_ball = false;
        for (int i = 0; i < MAX_BALLS; ++i)
            if (balls[i].active) any_ball = true;

        if (!any_ball) {
            if (--lives <= 0) {
                finished = true;
                return;
            }
            attach_ball();
        }
    }

    for (int i = 0; i < MAX_POWERUPS; ++i) {
        PowerUp& p = powerups[i];
        if (!p.active) continue;

        p.y += POWERUP_SPEED;

        if (p.y + POWERUP_SIZE >= paddle_y &&
            p.y <= paddle_y + PADDLE_H &&
            p.x + POWERUP_SIZE / 2 >= paddle_x &&
            p.x - POWERUP_SIZE / 2 <= paddle_x + paddle_w) {

            p.active = false;

            for (int j = 0; j < MAX_BALLS; ++j) {
                if (!balls[j].active) continue;
                int x = balls[j].x;
                int y = balls[j].y;
                spawn_duplicate_ball(x, y);
                break;
            }
        } else if (p.y - POWERUP_SIZE / 2 >= sh) {
            p.active = false;
        }
    }

    if (remaining <= 0) {
        remaining = 0;
        finished = true;
        won = true;
        return;
    }
}
}

void run() {
    if (!Kernel::Drivers::Framebuffer::valid()) return;
    uint32_t screen_w = 0, screen_h = 0;
    if (!Kernel::AppCommon::request_window_size(640, 480, screen_w, screen_h)) return;
    sw = static_cast<int>(screen_w);
    sh = static_cast<int>(screen_h);
    if (sw < 240 || sh < 220) return;

    if (!vfb.init(0, 0, static_cast<uint32_t>(sw), static_cast<uint32_t>(sh))) return;
    reset_game();

    bool space_prev = false;

    for (;;) {
        if (Kernel::Scheduler::should_stop()) return;
        if (!Kernel::Graphics::WindowManager::has_keyboard_focus(
                Kernel::Scheduler::current_thread_id())) {
            Kernel::AppCommon::delay(10);
            continue;
        }
        if (Keyboard::is_pressed(KEY_Q)) return;

        if (Keyboard::is_pressed(KEY_A) || Keyboard::is_pressed(KEY_J))
            paddle_x -= 6;
        if (Keyboard::is_pressed(KEY_D) || Keyboard::is_pressed(KEY_L))
            paddle_x += 6;

        bool space_curr = Keyboard::is_pressed(KEY_SPACE);
        bool space_clicked = space_curr && !space_prev;
        space_prev = space_curr;

        if (space_clicked) {
            if (finished) {
                reset_game();
            } else if (!launched) {
                launched = true;
                for (int i = 0; i < MAX_BALLS; ++i) {
                    if (!balls[i].active) continue;
                    balls[i].vx = 2;
                    balls[i].vy = -3;
                }
            }
        }

        paddle_x = clamp(paddle_x, 4, sw - paddle_w - 4);
        if (!finished) update();
        draw();
        Kernel::AppCommon::delay(2);
    }
}
}
