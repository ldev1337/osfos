#pragma once
#include <stdint.h>
#include <stddef.h>
#include "kernel/drivers/framebuffer.h"

namespace Kernel::Graphics {

class VirtualFramebuffer {
private:
    int32_t x_;
    int32_t y_;
    uint32_t width_;
    uint32_t height_;
    uint32_t* buffer_;
    uint32_t pitch_;
    bool initialized_;

public:
    VirtualFramebuffer();
    VirtualFramebuffer(int32_t x, int32_t y, uint32_t width, uint32_t height);
    ~VirtualFramebuffer();

    VirtualFramebuffer(const VirtualFramebuffer&) = delete;
    VirtualFramebuffer& operator=(const VirtualFramebuffer&) = delete;

    VirtualFramebuffer(VirtualFramebuffer&& other) noexcept;
    VirtualFramebuffer& operator=(VirtualFramebuffer&& other) noexcept;

    bool init(int32_t x, int32_t y, uint32_t width, uint32_t height);
    void destroy();

    bool valid() const { return initialized_ && buffer_ != nullptr; }
    int32_t x() const { return x_; }
    int32_t y() const { return y_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t pitch() const { return pitch_; }
    uint32_t* buffer() { return buffer_; }
    const uint32_t* buffer() const { return buffer_; }

    void flush() const;

    
    void put_pixel(uint32_t px, uint32_t py, uint32_t color);
    void clear(uint32_t color);
    void fill(uint32_t color);
    void fill_rect(int32_t rx, int32_t ry, int32_t rw, int32_t rh, uint32_t color);
    void draw_rect(int32_t rx, int32_t ry, int32_t w, int32_t h, uint32_t color);
    void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
    void draw_circle(int32_t xc, int32_t yc, int32_t r, uint32_t color);
    void fill_circle(int32_t xc, int32_t yc, int32_t r, uint32_t color);
    void fill_rect_alpha(int32_t rx, int32_t ry, int32_t w, int32_t h, uint32_t color, uint8_t alpha);
    void fill_circle_alpha(int32_t xc, int32_t yc, int32_t r, uint32_t color, uint8_t alpha);

    
    void draw_glyph(int32_t gx, int32_t gy, char ch, uint32_t color);
    void draw_glyph_bg(int32_t gx, int32_t gy, char ch, uint32_t fg, uint32_t bg);

    
    void blit(const uint32_t* src, uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh, uint32_t src_pitch);
    void blit_transparent(const uint32_t* src, uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh, uint32_t src_pitch, uint32_t transparent_color);
    void blit_alpha(const uint32_t* src, uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh, uint32_t src_pitch, uint8_t global_alpha);
    void draw_arc(int32_t xc, int32_t yc, int32_t r, int32_t start_angle, int32_t end_angle, uint32_t color);
    void draw_semicircle(int32_t xc, int32_t yc, int32_t r, int32_t angle_deg, uint32_t color);
    void fill_sector(int32_t xc, int32_t yc, int32_t r, int32_t start_angle, int32_t end_angle, uint32_t color);
    void draw_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color);
    void fill_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color);
    void draw_rounded_rect(int32_t rx, int32_t ry, int32_t w, int32_t h, int32_t radius, uint32_t color);
    void fill_rounded_rect(int32_t rx, int32_t ry, int32_t w, int32_t h, int32_t radius, uint32_t color);
};

} 