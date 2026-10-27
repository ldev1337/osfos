#include <kernel/graphics/vfb.h>
#include <kernel/graphics/window_manager.h>
#include <kernel/scheduler.h>
#include <kernel/graphics/font8x8.h>
#include <kernel/memory/heap.h>
#include <arch/x86_64/string.h>
#include <kernel/lib/math.h>
#include <utility>

namespace Kernel::Graphics {

VirtualFramebuffer::VirtualFramebuffer()
    : x_(0), y_(0), width_(0), height_(0), buffer_(nullptr), pitch_(0), initialized_(false) {}

VirtualFramebuffer::VirtualFramebuffer(int32_t x, int32_t y, uint32_t width, uint32_t height)
    : x_(0), y_(0), width_(0), height_(0), buffer_(nullptr), pitch_(0), initialized_(false) {
    init(x, y, width, height);
}

VirtualFramebuffer::~VirtualFramebuffer() {
    destroy();
}

VirtualFramebuffer::VirtualFramebuffer(VirtualFramebuffer&& other) noexcept
    : x_(other.x_), y_(other.y_), width_(other.width_), height_(other.height_),
      buffer_(other.buffer_), pitch_(other.pitch_), initialized_(other.initialized_) {
    other.buffer_ = nullptr;
    other.initialized_ = false;
    other.width_ = 0;
    other.height_ = 0;
}

VirtualFramebuffer& VirtualFramebuffer::operator=(VirtualFramebuffer&& other) noexcept {
    if (this != &other) {
        destroy();
        x_ = other.x_;
        y_ = other.y_;
        width_ = other.width_;
        height_ = other.height_;
        buffer_ = other.buffer_;
        pitch_ = other.pitch_;
        initialized_ = other.initialized_;

        other.buffer_ = nullptr;
        other.initialized_ = false;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

bool VirtualFramebuffer::init(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    destroy();
    if (width == 0 || height == 0) return false;

    x_ = x;
    y_ = y;
    width_ = width;
    height_ = height;
    pitch_ = width * sizeof(uint32_t);

    size_t size = static_cast<size_t>(height_) * pitch_;
    buffer_ = static_cast<uint32_t*>(kmalloc(size));
    if (!buffer_) return false;

    memset(buffer_, 0, size);
    initialized_ = true;
    return true;
}

void VirtualFramebuffer::destroy() {
    if (buffer_ && initialized_) {
        kfree(buffer_);
        buffer_ = nullptr;
    }
    initialized_ = false;
    width_ = 0;
    height_ = 0;
}

void VirtualFramebuffer::flush() const {
    if (!initialized_ || !buffer_) return;
    if (Kernel::Graphics::WindowManager::present(
            Kernel::Scheduler::current_thread_id(), buffer_, width_, height_, pitch_)) return;
    Kernel::Drivers::Framebuffer::blit(buffer_, static_cast<uint32_t>(x_), static_cast<uint32_t>(y_), width_, height_, pitch_);
}

void VirtualFramebuffer::put_pixel(uint32_t px, uint32_t py, uint32_t color) {
    if (!initialized_ || px >= width_ || py >= height_) [[unlikely]] return;
    buffer_[py * width_ + px] = color;
}

void VirtualFramebuffer::clear(uint32_t color) {
    fill(color);
}

void VirtualFramebuffer::fill(uint32_t color) {
    if (!initialized_ || !buffer_) return;
    fill_rect(0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_), color);
}

void VirtualFramebuffer::fill_rect(int32_t rx, int32_t ry, int32_t rw, int32_t rh, uint32_t color) {
    if (!initialized_ || !buffer_) return;
    const int32_t sw = static_cast<int32_t>(width_);
    const int32_t sh = static_cast<int32_t>(height_);
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > sw) rw = sw - rx;
    if (ry + rh > sh) rh = sh - ry;
    if (rw <= 0 || rh <= 0) return;

    const uint32_t ux = static_cast<uint32_t>(rx);
    const uint32_t uy = static_cast<uint32_t>(ry);
    const uint32_t uw = static_cast<uint32_t>(rw);
    const uint32_t uh = static_cast<uint32_t>(rh);

    for (uint32_t py = 0; py < uh; ++py) {
        uint32_t* row = buffer_ + (uy + py) * width_ + ux;
        uint32_t px = 0;
        const uint64_t pair = uint64_t(color) | (uint64_t(color) << 32);
        for (; px + 1 < uw; px += 2)
            *reinterpret_cast<uint64_t*>(row + px) = pair;
        if (px < uw) row[px] = color;
    }
}

void VirtualFramebuffer::draw_rect(int32_t rx, int32_t ry, int32_t w, int32_t h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    fill_rect(rx, ry, w, 1, color);
    fill_rect(rx, ry + h - 1, w, 1, color);
    fill_rect(rx, ry, 1, h, color);
    fill_rect(rx + w - 1, ry, 1, h, color);
}

void VirtualFramebuffer::draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    if (!initialized_) return;
    int32_t dx = __builtin_abs(x1 - x0);
    int32_t dy = __builtin_abs(y1 - y0);
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx - dy;

    while (true) {
        if (static_cast<uint32_t>(x0) < width_ && static_cast<uint32_t>(y0) < height_) {
            put_pixel(static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), color);
        }
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void VirtualFramebuffer::draw_circle(int32_t xc, int32_t yc, int32_t r, uint32_t color) {
    if (!initialized_ || r < 0) return;
    int32_t x = 0, y = r, d = 3 - 2 * r;
    auto plot_sym = [&](int32_t px, int32_t py) {
        int32_t xs[8] = {xc + px, xc - px, xc + px, xc - px, xc + py, xc - py, xc + py, xc - py};
        int32_t ys[8] = {yc + py, yc + py, yc - py, yc - py, yc + px, yc + px, yc - px, yc - px};
        for (int i = 0; i < 8; ++i) {
            if (static_cast<uint32_t>(xs[i]) < width_ && static_cast<uint32_t>(ys[i]) < height_) {
                put_pixel(xs[i], ys[i], color);
            }
        }
    };
    while (y >= x) {
        plot_sym(x, y);
        x++;
        if (d > 0) { y--; d += 4 * (x - y) + 10; }
        else { d += 4 * x + 6; }
    }
}

template <typename F_func>
inline void process_circle_spans(int32_t xc, int32_t yc, int32_t r, F_func&& span_func) {
    if (r < 0) return;
    const int64_t rr = int64_t(r) * r;
    int32_t dx = r;
    for (int32_t dy = 0; dy <= r; ++dy) {
        const int64_t yy = int64_t(dy) * dy;
        while (dx > 0 && int64_t(dx) * dx + yy > rr) --dx;
        const int64_t left = int64_t(xc) - dx;
        const int64_t bottom = int64_t(yc) + dy;
        const int64_t top = int64_t(yc) - dy;
        const int64_t width = 2LL * dx + 1;
        if (left < INT32_MIN || (left + width) > INT32_MAX || top < INT32_MIN || bottom > INT32_MAX) continue;
        span_func(static_cast<int32_t>(left), static_cast<int32_t>(bottom), static_cast<int32_t>(width));
        if (dy != 0) {
            span_func(static_cast<int32_t>(left), static_cast<int32_t>(top), static_cast<int32_t>(width));
        }
    }
}

void VirtualFramebuffer::fill_circle(int32_t xc, int32_t yc, int32_t r, uint32_t color) {
    process_circle_spans(xc, yc, r, [&](int32_t l, int32_t t, int32_t w) {
        fill_rect(l, t, w, 1, color);
    });
}

inline uint32_t blend_pixel_32(uint32_t fg, uint32_t bg, uint8_t alpha) {
    if (alpha == 255) return fg;
    if (alpha == 0) return bg;
    uint8_t fr = (fg >> 16) & 0xFF;
    uint8_t fg_g = (fg >> 8) & 0xFF;
    uint8_t fb_c = fg & 0xFF;
    uint8_t br = (bg >> 16) & 0xFF;
    uint8_t bg_g = (bg >> 8) & 0xFF;
    uint8_t bb = bg & 0xFF;

    uint8_t inv = 255 - alpha;
    uint8_t or_c = (fr * alpha + br * inv + 128) >> 8;
    uint8_t og_c = (fg_g * alpha + bg_g * inv + 128) >> 8;
    uint8_t ob_c = (fb_c * alpha + bb * inv + 128) >> 8;

    return (uint32_t(or_c) << 16) | (uint32_t(og_c) << 8) | ob_c;
}

void VirtualFramebuffer::fill_rect_alpha(int32_t rx, int32_t ry, int32_t w, int32_t h, uint32_t color, uint8_t alpha) {
    if (alpha == 0) return;
    if (alpha == 255) { fill_rect(rx, ry, w, h, color); return; }
    if (!initialized_) return;

    int32_t sw = static_cast<int32_t>(width_);
    int32_t sh = static_cast<int32_t>(height_);
    if (rx < 0) { w += rx; rx = 0; }
    if (ry < 0) { h += ry; ry = 0; }
    if (rx + w > sw) w = sw - rx;
    if (ry + h > sh) h = sh - ry;
    if (w <= 0 || h <= 0) return;

    for (int32_t py = 0; py < h; ++py) {
        uint32_t* row = buffer_ + (ry + py) * width_ + rx;
        for (int32_t px = 0; px < w; ++px) {
            row[px] = blend_pixel_32(color, row[px], alpha);
        }
    }
}

void VirtualFramebuffer::fill_circle_alpha(int32_t xc, int32_t yc, int32_t r, uint32_t color, uint8_t alpha) {
    if (alpha == 255) { fill_circle(xc, yc, r, color); return; }
    process_circle_spans(xc, yc, r, [&](int32_t l, int32_t t, int32_t w) {
        fill_rect_alpha(l, t, w, 1, color, alpha);
    });
}

void VirtualFramebuffer::draw_glyph(int32_t gx, int32_t gy, char ch, uint32_t color) {
    if (!initialized_) return;
    unsigned char c = static_cast<unsigned char>(ch);
    if (c < 32 || c > 127) c = '?';
    const uint8_t* bits = Kernel::Graphics::Font::Basic8x8[c - 32];
    for (int32_t py = 0; py < 8; ++py) {
        int32_t cy = gy + py;
        if (cy < 0 || cy >= static_cast<int32_t>(height_)) continue;
        uint8_t row_bits = bits[py];
        for (int32_t px = 0; px < 8; ++px) {
            if (row_bits & (1u << px)) {
                int32_t cx = gx + px;
                if (cx >= 0 && cx < static_cast<int32_t>(width_)) {
                    put_pixel(static_cast<uint32_t>(cx), static_cast<uint32_t>(cy), color);
                }
            }
        }
    }
}

void VirtualFramebuffer::draw_glyph_bg(int32_t gx, int32_t gy, char ch, uint32_t fg, uint32_t bg) {
    if (!initialized_) return;
    unsigned char c = static_cast<unsigned char>(ch);
    if (c < 32 || c > 127) c = '?';
    const uint8_t* bits = Kernel::Graphics::Font::Basic8x8[c - 32];
    for (int32_t py = 0; py < 8; ++py) {
        int32_t cy = gy + py;
        if (cy < 0 || cy >= static_cast<int32_t>(height_)) continue;
        uint8_t row_bits = bits[py];
        for (int32_t px = 0; px < 8; ++px) {
            int32_t cx = gx + px;
            if (cx >= 0 && cx < static_cast<int32_t>(width_)) {
                put_pixel(static_cast<uint32_t>(cx), static_cast<uint32_t>(cy), (row_bits & (1u << px)) ? fg : bg);
            }
        }
    }
}

void VirtualFramebuffer::blit(const uint32_t* src, uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh, uint32_t src_pitch) {
    if (!initialized_ || !src || bx >= width_ || by >= height_) return;
    if (bw > width_ - bx) bw = width_ - bx;
    if (bh > height_ - by) bh = height_ - by;
    if (bw == 0 || bh == 0) return;

    const uint8_t* src_curr = reinterpret_cast<const uint8_t*>(src);
    uint32_t* dst_curr = buffer_ + by * width_ + bx;

    for (uint32_t row = 0; row < bh; ++row) {
        memcpy(dst_curr, src_curr, static_cast<size_t>(bw) * sizeof(uint32_t));
        dst_curr += width_;
        src_curr += src_pitch;
    }
}

void VirtualFramebuffer::blit_transparent(const uint32_t* src, uint32_t bx, uint32_t by,
                                          uint32_t bw, uint32_t bh, uint32_t src_pitch, uint32_t transparent_color) {
    if (!initialized_ || !src || bx >= width_ || by >= height_) return;
    if (bw > width_ - bx) bw = width_ - bx;
    if (bh > height_ - by) bh = height_ - by;
    if (bw == 0 || bh == 0) return;

    const uint8_t* src_curr = reinterpret_cast<const uint8_t*>(src);
    uint32_t* dst_curr = buffer_ + by * width_ + bx;

    for (uint32_t row = 0; row < bh; ++row) {
        auto* s = reinterpret_cast<const uint32_t*>(src_curr);
        for (uint32_t px = 0; px < bw; ++px) {
            if (s[px] != transparent_color) {
                dst_curr[px] = s[px];
            }
        }
        dst_curr += width_;
        src_curr += src_pitch;
    }
}

void VirtualFramebuffer::blit_alpha(const uint32_t* src, uint32_t bx, uint32_t by,
                                    uint32_t bw, uint32_t bh, uint32_t src_pitch, uint8_t global_alpha) {
    if (global_alpha == 0 || !initialized_ || !src) return;
    if (bx >= width_ || by >= height_) return;
    if (bw > width_ - bx) bw = width_ - bx;
    if (bh > height_ - by) bh = height_ - by;
    if (bw == 0 || bh == 0) return;

    const uint8_t* src_curr = reinterpret_cast<const uint8_t*>(src);
    uint32_t* dst_curr = buffer_ + by * width_ + bx;

    for (uint32_t row = 0; row < bh; ++row) {
        auto* s = reinterpret_cast<const uint32_t*>(src_curr);
        for (uint32_t px = 0; px < bw; ++px) {
            dst_curr[px] = blend_pixel_32(s[px], dst_curr[px], global_alpha);
        }
        dst_curr += width_;
        src_curr += src_pitch;
    }
}

void VirtualFramebuffer::draw_arc(int32_t xc, int32_t yc, int32_t r, int32_t start_angle, int32_t end_angle, uint32_t color) {
    if (!initialized_ || r < 0) return;
    bool first = true;
    int32_t prev_x = 0, prev_y = 0;
    for (int32_t deg = start_angle; deg <= end_angle; ++deg) {
        int32_t x = xc + (r * Kernel::Lib::Math::fixed_cos(deg)) / 1024;
        int32_t y = yc + (r * Kernel::Lib::Math::fixed_sin(deg)) / 1024;
        if (first) {
            prev_x = x; prev_y = y; first = false;
        } else {
            draw_line(prev_x, prev_y, x, y, color);
            prev_x = x; prev_y = y;
        }
    }
}

void VirtualFramebuffer::draw_semicircle(int32_t xc, int32_t yc, int32_t r, int32_t angle_deg, uint32_t color) {
    draw_arc(xc, yc, r, angle_deg, angle_deg + 180, color);
}

void VirtualFramebuffer::fill_sector(int32_t xc, int32_t yc, int32_t r, int32_t start_angle, int32_t end_angle, uint32_t color) {
    if (!initialized_ || r < 0) return;
    bool first = true;
    int32_t prev_x = 0, prev_y = 0;
    for (int32_t deg = start_angle; deg <= end_angle; ++deg) {
        int32_t x = xc + (r * Kernel::Lib::Math::fixed_cos(deg)) / 1024;
        int32_t y = yc + (r * Kernel::Lib::Math::fixed_sin(deg)) / 1024;
        if (first) {
            prev_x = x; prev_y = y; first = false;
        } else {
            fill_triangle(xc, yc, prev_x, prev_y, x, y, color);
            prev_x = x; prev_y = y;
        }
    }
}

void VirtualFramebuffer::draw_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color) {
    draw_line(x0, y0, x1, y1, color);
    draw_line(x1, y1, x2, y2, color);
    draw_line(x2, y2, x0, y0, color);
}

void VirtualFramebuffer::fill_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color) {
    if (!initialized_) return;
    if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); }
    if (y1 > y2) { std::swap(x1, x2); std::swap(y1, y2); }
    if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); }
    if (y0 == y2) return;

    const int32_t sh = static_cast<int32_t>(height_);
    if (y2 < 0 || y0 >= sh) return;

    const int64_t dx_long = ((int64_t(x2) - x0) * 65536) / (int64_t(y2) - y0);
    const int64_t max_x = static_cast<int64_t>(width_);

    auto draw_span = [&](int32_t y, int64_t xa, int64_t xb) {
        xa >>= 16; xb >>= 16;
        if (xa > xb) std::swap(xa, xb);
        if (xb < 0 || xa >= max_x) return;
        if (xa < 0) xa = 0;
        if (xb >= max_x) xb = max_x - 1;

        uint32_t* dst = buffer_ + static_cast<size_t>(y) * width_ + xa;
        for (int64_t x = xa; x <= xb; ++x) *dst++ = color;
    };

    if (y0 < y1) {
        const int64_t dx_top = ((int64_t(x1) - x0) * 65536) / (int64_t(y1) - y0);
        int32_t start = y0 < 0 ? 0 : y0;
        int32_t end = y1 >= sh ? sh - 1 : y1;

        if (start <= end) {
            int64_t lx = int64_t(x0) * 65536 + dx_long * (int64_t(start) - y0);
            int64_t sx = int64_t(x0) * 65536 + dx_top * (int64_t(start) - y0);
            for (int32_t y = start; y <= end; ++y) {
                draw_span(y, lx, sx);
                lx += dx_long; sx += dx_top;
            }
        }
    }

    if (y1 < y2) {
        const int64_t dx_bottom = ((int64_t(x2) - x1) * 65536) / (int64_t(y2) - y1);
        int32_t start = (y0 == y1) ? y1 : y1 + 1;
        if (start < 0) start = 0;
        int32_t end = y2 >= sh ? sh - 1 : y2;

        if (start <= end) {
            int64_t lx = int64_t(x0) * 65536 + dx_long * (int64_t(start) - y0);
            int64_t sx = int64_t(x1) * 65536 + dx_bottom * (int64_t(start) - y1);
            for (int32_t y = start; y <= end; ++y) {
                draw_span(y, lx, sx);
                lx += dx_long; sx += dx_bottom;
            }
        }
    }
}

void VirtualFramebuffer::draw_rounded_rect(int32_t rx, int32_t ry, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    if (radius <= 0) { draw_rect(rx, ry, w, h, color); return; }
    draw_line(rx + radius, ry, rx + w - radius - 1, ry, color);
    draw_line(rx + radius, ry + h - 1, rx + w - radius - 1, ry + h - 1, color);
    draw_line(rx, ry + radius, rx, ry + h - radius - 1, color);
    draw_line(rx + w - 1, ry + radius, rx + w - 1, ry + h - radius - 1, color);

    draw_arc(rx + radius, ry + radius, radius, 180, 270, color);
    draw_arc(rx + w - radius - 1, ry + radius, radius, 270, 360, color);
    draw_arc(rx + radius, ry + h - radius - 1, radius, 90, 180, color);
    draw_arc(rx + w - radius - 1, ry + h - radius - 1, radius, 0, 90, color);
}

void VirtualFramebuffer::fill_rounded_rect(int32_t rx, int32_t ry, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    if (radius <= 0) { fill_rect(rx, ry, w, h, color); return; }
    fill_rect(rx + radius, ry, w - 2 * radius, h, color);
    fill_rect(rx, ry + radius, radius, h - 2 * radius, color);
    fill_rect(rx + w - radius, ry + radius, radius, h - 2 * radius, color);

    for (int32_t dy = 0; dy < radius; dy++) {
        int32_t dy2 = (radius - dy) * (radius - dy);
        int32_t dx = 0;
        while ((dx + 1) * (dx + 1) + dy2 <= radius * radius) dx++;
        fill_rect(rx + radius - dx, ry + dy, dx + 1, 1, color);
        fill_rect(rx + w - radius, ry + dy, dx + 1, 1, color);
        fill_rect(rx + radius - dx, ry + h - 1 - dy, dx + 1, 1, color);
        fill_rect(rx + w - radius, ry + h - 1 - dy, dx + 1, 1, color);
    }
}

}
