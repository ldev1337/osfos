#include <kernel/graphics/vfb.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/fs/vfs.h>
#include <kernel/memory/heap.h>
#include <kernel/lib/appcommon.h>
#include <kernel/lib/uicommon.h>
#include <kernel/scheduler.h>
#include <arch/x86_64/string.h>

using Kernel::Drivers::Framebuffer::width;
using Kernel::Drivers::Framebuffer::height;
using Kernel::Drivers::Mouse;

namespace Kernel::Apps::Pain {

enum class Tool {
    Pencil,
    Eraser,
    Line,
    Rect,
    FillRect,
    Circle,
    FillCircle,
    FloodFill,
    Eyedropper
};

inline uint32_t calculate_crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
    }
    return ~crc;
}

inline uint32_t calculate_adler32(const uint8_t* data, size_t length) {
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < length; ++i) {
        s1 = (s1 + data[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    return (s2 << 16) | s1;
}

inline bool save_png(const char* filename, const uint32_t* pixels, uint32_t width, uint32_t height) {
    void* f = Kernel::FS::VFS::vfs_open(filename, "w+");
    if (!f) return false;

    const uint8_t png_sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    Kernel::FS::VFS::vfs_write(png_sig, 1, 8, f);

    uint8_t ihdr_data[13] = {
        static_cast<uint8_t>(width >> 24), static_cast<uint8_t>(width >> 16),
        static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width),
        static_cast<uint8_t>(height >> 24), static_cast<uint8_t>(height >> 16),
        static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height),
        8, 6, 0, 0, 0
    };

    uint32_t net_len = __builtin_bswap32(13);
    uint8_t ihdr_type[4] = { 'I', 'H', 'D', 'R' };
    Kernel::FS::VFS::vfs_write(&net_len, 1, 4, f);
    Kernel::FS::VFS::vfs_write(ihdr_type, 1, 4, f);
    Kernel::FS::VFS::vfs_write(ihdr_data, 1, 13, f);

    uint8_t crc_buf[17];
    memcpy(crc_buf, ihdr_type, 4);
    memcpy(crc_buf + 4, ihdr_data, 13);
    uint32_t crc = __builtin_bswap32(calculate_crc32(crc_buf, 17));
    Kernel::FS::VFS::vfs_write(&crc, 1, 4, f);

    size_t row_size = width * 4 + 1;
    size_t total_idat_data = height * row_size;
    uint8_t* raw_filtered = static_cast<uint8_t*>(kmalloc(total_idat_data));
    size_t raw_offset = 0;

    for (uint32_t y = 0; y < height; ++y) {
        raw_filtered[raw_offset++] = 0;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t p = pixels[y * width + x];
            raw_filtered[raw_offset++] = (p >> 16) & 0xFF;
            raw_filtered[raw_offset++] = (p >> 8) & 0xFF;
            raw_filtered[raw_offset++] = p & 0xFF;
            raw_filtered[raw_offset++] = (p >> 24) & 0xFF;
        }
    }

    size_t num_blocks = (total_idat_data + 65534) / 65535;
    size_t zlib_size = 2 + total_idat_data + (num_blocks * 5) + 4;
    uint8_t* zlib_buf = static_cast<uint8_t*>(kmalloc(zlib_size));
    size_t z_idx = 0;
    zlib_buf[z_idx++] = 0x78;
    zlib_buf[z_idx++] = 0x01;

    size_t remaining = total_idat_data, src_offset = 0;
    while (remaining > 0) {
        size_t block_len = remaining > 65535 ? 65535 : remaining;
        remaining -= block_len;
        bool final = (remaining == 0);

        zlib_buf[z_idx++] = final ? 0x01 : 0x00;
        zlib_buf[z_idx++] = block_len & 0xFF;
        zlib_buf[z_idx++] = (block_len >> 8) & 0xFF;
        uint16_t nl = ~((uint16_t)block_len);
        zlib_buf[z_idx++] = nl & 0xFF;
        zlib_buf[z_idx++] = (nl >> 8) & 0xFF;

        memcpy(zlib_buf + z_idx, raw_filtered + src_offset, block_len);
        z_idx += block_len;
        src_offset += block_len;
    }
    kfree(raw_filtered);

    uint32_t adler = calculate_adler32(zlib_buf + 2, z_idx - 2);
    zlib_buf[z_idx++] = (adler >> 24) & 0xFF;
    zlib_buf[z_idx++] = (adler >> 16) & 0xFF;
    zlib_buf[z_idx++] = (adler >> 8) & 0xFF;
    zlib_buf[z_idx++] = adler & 0xFF;

    uint32_t net_idat_len = __builtin_bswap32(z_idx);
    uint8_t idat_type[4] = { 'I', 'D', 'A', 'T' };
    Kernel::FS::VFS::vfs_write(&net_idat_len, 1, 4, f);
    Kernel::FS::VFS::vfs_write(idat_type, 1, 4, f);
    Kernel::FS::VFS::vfs_write(zlib_buf, 1, z_idx, f);

    uint8_t* idat_crc_buf = static_cast<uint8_t*>(kmalloc(4 + z_idx));
    memcpy(idat_crc_buf, idat_type, 4);
    memcpy(idat_crc_buf + 4, zlib_buf, z_idx);
    uint32_t idat_crc = __builtin_bswap32(calculate_crc32(idat_crc_buf, 4 + z_idx));
    Kernel::FS::VFS::vfs_write(&idat_crc, 1, 4, f);
    kfree(idat_crc_buf);
    kfree(zlib_buf);

    uint32_t zero_len = 0;
    uint8_t iend_type[4] = { 'I', 'E', 'N', 'D' };
    Kernel::FS::VFS::vfs_write(&zero_len, 1, 4, f);
    Kernel::FS::VFS::vfs_write(iend_type, 1, 4, f);
    uint32_t iend_crc = __builtin_bswap32(0xAE426082);
    Kernel::FS::VFS::vfs_write(&iend_crc, 1, 4, f);

    Kernel::FS::VFS::vfs_close(f);
    return true;
}

inline bool load_png(const char* filename, uint32_t* pixels, uint32_t width, uint32_t height) {
    void* f = Kernel::FS::VFS::vfs_open(filename, "r");
    if (!f) return false;

    size_t filesize = Kernel::FS::VFS::vfs_size(f);
    if (filesize < 33) { Kernel::FS::VFS::vfs_close(f); return false; }

    uint8_t* file_data = static_cast<uint8_t*>(kmalloc(filesize));
    Kernel::FS::VFS::vfs_read(file_data, 1, filesize, f);
    Kernel::FS::VFS::vfs_close(f);

    const uint8_t png_sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (memcmp(file_data, png_sig, 8) != 0) {
        kfree(file_data);
        return false;
    }

    size_t offset = 8;
    bool success = false;

    while (offset < filesize) {
        uint32_t chunk_len = __builtin_bswap32(*reinterpret_cast<uint32_t*>(file_data + offset));
        offset += 4;
        char chunk_type[5];
        memcpy(chunk_type, file_data + offset, 4);
        chunk_type[4] = '\0';
        offset += 4;

        if (strcmp(chunk_type, "IDAT") == 0) {
            const uint8_t* zdata = file_data + offset;
            size_t z_pos = 2, z_end = chunk_len;
            size_t row_size = width * 4 + 1;
            uint8_t* raw_filtered = static_cast<uint8_t*>(kmalloc(height * row_size));
            size_t raw_pos = 0;

            while (z_pos < z_end - 4) {
                uint8_t hdr = zdata[z_pos++];
                uint16_t block_len = zdata[z_pos] | (zdata[z_pos + 1] << 8);
                z_pos += 4;
                memcpy(raw_filtered + raw_pos, zdata + z_pos, block_len);
                raw_pos += block_len;
                z_pos += block_len;
                if (hdr & 1) break;
            }

            size_t rf_idx = 0;
            for (uint32_t y = 0; y < height; ++y) {
                rf_idx++;
                for (uint32_t x = 0; x < width; ++x) {
                    uint8_t r = raw_filtered[rf_idx++];
                    uint8_t g = raw_filtered[rf_idx++];
                    uint8_t b = raw_filtered[rf_idx++];
                    uint8_t a = raw_filtered[rf_idx++];
                    pixels[y * width + x] = (static_cast<uint32_t>(a) << 24) |
                                            (static_cast<uint32_t>(r) << 16) |
                                            (static_cast<uint32_t>(g) << 8) |
                                            static_cast<uint32_t>(b);
                }
            }
            kfree(raw_filtered);
            success = true;
        } else if (strcmp(chunk_type, "IEND") == 0) {
            break;
        }
        offset += chunk_len + 4;
    }

    kfree(file_data);
    return success;
}

inline void draw_line_pixels(uint32_t* canvas, uint32_t cw, uint32_t ch, int x0, int y0, int x1, int y1, uint32_t color, int width = 1) {
    int dx = __builtin_abs(x1 - x0);
    int dy = __builtin_abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        for (int wy = -width / 2; wy <= width / 2; ++wy) {
            for (int wx = -width / 2; wx <= width / 2; ++wx) {
                int px = x0 + wx;
                int py = y0 + wy;
                if (px >= 0 && px < static_cast<int>(cw) && py >= 0 && py < static_cast<int>(ch)) {
                    canvas[py * cw + px] = color;
                }
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

inline void draw_rect_pixels(uint32_t* canvas, uint32_t cw, uint32_t ch, int x0, int y0, int x1, int y1, uint32_t color, bool filled, int width = 1) {
    int min_x = x0 < x1 ? x0 : x1;
    int max_x = x0 > x1 ? x0 : x1;
    int min_y = y0 < y1 ? y0 : y1;
    int max_y = y0 > y1 ? y0 : y1;

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            if (x >= 0 && x < static_cast<int>(cw) && y >= 0 && y < static_cast<int>(ch)) {
                if (filled || x < min_x + width || x > max_x - width || y < min_y + width || y > max_y - width) {
                    canvas[y * cw + x] = color;
                }
            }
        }
    }
}

inline void draw_circle_pixels(uint32_t* canvas, uint32_t cw, uint32_t ch, int xc, int yc, int r, uint32_t color, bool filled, int width = 1) {
    if (r < 0) r = -r;
    int x = 0;
    int y = r;
    int d = 3 - 2 * r;

    auto plot_symmetric = [&](int cx, int cy, int px, int py) {
        int points[8][2] = {
            {cx + px, cy + py}, {cx - px, cy + py}, {cx + px, cy - py}, {cx - px, cy - py},
            {cx + py, cy + px}, {cx - py, cy + px}, {cx + py, cy - px}, {cx - py, cy - px}
        };
        for (auto& pt : points) {
            for (int wy = -width / 2; wy <= width / 2; ++wy) {
                for (int wx = -width / 2; wx <= width / 2; ++wx) {
                    int fx = pt[0] + wx;
                    int fy = pt[1] + wy;
                    if (fx >= 0 && fx < static_cast<int>(cw) && fy >= 0 && fy < static_cast<int>(ch)) {
                        canvas[fy * cw + fx] = color;
                    }
                }
            }
        }
    };

    while (y >= x) {
        if (filled) {
            draw_line_pixels(canvas, cw, ch, xc - x, yc + y, xc + x, yc + y, color, 1);
            draw_line_pixels(canvas, cw, ch, xc - x, yc - y, xc + x, yc - y, color, 1);
            draw_line_pixels(canvas, cw, ch, xc - y, yc + x, xc + y, yc + x, color, 1);
            draw_line_pixels(canvas, cw, ch, xc - y, yc - x, xc + y, yc - x, color, 1);
        } else {
            plot_symmetric(xc, yc, x, y);
        }
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

inline void flood_fill(uint32_t* canvas, uint32_t cw, uint32_t ch, int start_x, int start_y, uint32_t new_color) {
    if (start_x < 0 || start_x >= static_cast<int>(cw) || start_y < 0 || start_y >= static_cast<int>(ch)) return;
    uint32_t target_color = canvas[start_y * cw + start_x];
    if (target_color == new_color) return;

    struct Point { int x, y; };
    size_t stack_capacity = cw * ch;
    Point* stack = static_cast<Point*>(kmalloc(stack_capacity * sizeof(Point)));
    size_t top = 0;

    stack[top++] = {start_x, start_y};

    while (top > 0) {
        Point p = stack[--top];
        int x = p.x, y = p.y;

        if (x < 0 || x >= static_cast<int>(cw) || y < 0 || y >= static_cast<int>(ch)) continue;
        if (canvas[y * cw + x] != target_color) continue;

        canvas[y * cw + x] = new_color;

        if (top + 4 < stack_capacity) {
            stack[top++] = {x + 1, y};
            stack[top++] = {x - 1, y};
            stack[top++] = {x, y + 1};
            stack[top++] = {x, y - 1};
        }
    }
    kfree(stack);
}

void run() {
    uint32_t actual_w = 0, actual_h = 0;
    if (!Kernel::AppCommon::request_window_size(1024, 720, actual_w, actual_h)) return;
    int32_t sw = static_cast<int32_t>(actual_w), sh = static_cast<int32_t>(actual_h);
    const uint32_t canvas_w = sw, canvas_h = sh > 100 ? sh - 100 : sh;
    Kernel::Graphics::VirtualFramebuffer vfb(0, 0, static_cast<uint32_t>(sw), static_cast<uint32_t>(sh));

    const uint32_t COLOR_BG = 0xFF1E1E2E, COLOR_CANVAS = 0xFFFFFFFF;
    const uint32_t COLOR_TEXT = 0xFFCDD6F4, COLOR_ACCENT = 0xFF89B4FA, COLOR_CURSOR = 0xFFF38BA8;

    uint32_t* canvas = static_cast<uint32_t*>(kmalloc(canvas_w * canvas_h * sizeof(uint32_t)));
    uint32_t* backup_canvas = static_cast<uint32_t*>(kmalloc(canvas_w * canvas_h * sizeof(uint32_t)));
    if (!canvas || !backup_canvas) { if (canvas) kfree(canvas); if (backup_canvas) kfree(backup_canvas); return; }
    for (size_t i = 0; i < canvas_w * canvas_h; ++i) canvas[i] = COLOR_CANVAS;
    memcpy(backup_canvas, canvas, canvas_w * canvas_h * sizeof(uint32_t));

    Tool current_tool = Tool::Pencil;
    uint8_t cur_r = 0, cur_g = 0, cur_b = 0;
    int brush_width = 2;
    char status_msg[64] = "Ready. Drag & drop tools enabled.";

    bool mouse_down_prev = false;
    int start_x = -1, start_y = -1;
    bool dragging = false;

    bool running = true;
    while (running && !Kernel::Scheduler::should_stop()) {
        int mx = Mouse::get_x();
        int my = Mouse::get_y();
        bool m_left = Mouse::is_left_pressed();

        if (my < static_cast<int>(canvas_h)) {
            if (m_left) {
                if (!mouse_down_prev) {
                    start_x = mx;
                    start_y = my;
                    dragging = true;
                    memcpy(backup_canvas, canvas, canvas_w * canvas_h * sizeof(uint32_t));

                    if (current_tool == Tool::FloodFill) {
                        flood_fill(canvas, canvas_w, canvas_h, mx, my, 0xFF000000 | (cur_r << 16) | (cur_g << 8) | cur_b);
                        memcpy(backup_canvas, canvas, canvas_w * canvas_h * sizeof(uint32_t));
                        strcpy(status_msg, "Filled area.");
                        dragging = false;
                    } else if (current_tool == Tool::Eyedropper) {
                        uint32_t col = canvas[my * canvas_w + mx];
                        cur_r = (col >> 16) & 0xFF;
                        cur_g = (col >> 8) & 0xFF;
                        cur_b = col & 0xFF;
                        strcpy(status_msg, "Picked color!");
                        dragging = false;
                    }
                } else {
                    if (current_tool == Tool::Pencil) {
                        draw_line_pixels(canvas, canvas_w, canvas_h, start_x, start_y, mx, my, 0xFF000000 | (cur_r << 16) | (cur_g << 8) | cur_b, brush_width);
                        start_x = mx;
                        start_y = my;
                        memcpy(backup_canvas, canvas, canvas_w * canvas_h * sizeof(uint32_t));
                    } else if (current_tool == Tool::Eraser) {
                        draw_line_pixels(canvas, canvas_w, canvas_h, start_x, start_y, mx, my, COLOR_CANVAS, brush_width * 2);
                        start_x = mx;
                        start_y = my;
                        memcpy(backup_canvas, canvas, canvas_w * canvas_h * sizeof(uint32_t));
                    } else if (current_tool == Tool::Line || current_tool == Tool::Rect || current_tool == Tool::FillRect || current_tool == Tool::Circle || current_tool == Tool::FillCircle) {
                        memcpy(canvas, backup_canvas, canvas_w * canvas_h * sizeof(uint32_t));
                        uint32_t col = 0xFF000000 | (cur_r << 16) | (cur_g << 8) | cur_b;
                        if (current_tool == Tool::Line) {
                            draw_line_pixels(canvas, canvas_w, canvas_h, start_x, start_y, mx, my, col, brush_width);
                        } else if (current_tool == Tool::Rect) {
                            draw_rect_pixels(canvas, canvas_w, canvas_h, start_x, start_y, mx, my, col, false, brush_width);
                        } else if (current_tool == Tool::FillRect) {
                            draw_rect_pixels(canvas, canvas_w, canvas_h, start_x, start_y, mx, my, col, true, brush_width);
                        } else if (current_tool == Tool::Circle) {
                            int r = __builtin_abs(mx - start_x);
                            draw_circle_pixels(canvas, canvas_w, canvas_h, start_x, start_y, r, col, false, brush_width);
                        } else if (current_tool == Tool::FillCircle) {
                            int r = __builtin_abs(mx - start_x);
                            draw_circle_pixels(canvas, canvas_w, canvas_h, start_x, start_y, r, col, true, brush_width);
                        }
                    }
                }
            } else {
                if (mouse_down_prev && dragging) {
                    dragging = false;
                    memcpy(backup_canvas, canvas, canvas_w * canvas_h * sizeof(uint32_t));
                    strcpy(status_msg, "Shape applied.");
                }
            }
        } else {
            if (m_left && !mouse_down_prev) {

                if (my >= static_cast<int>(canvas_h) + 5 && my <= static_cast<int>(canvas_h) + 30) {
                    if (mx >= 10 && mx < 70) { current_tool = Tool::Pencil; strcpy(status_msg, "Tool: Pencil"); }
                    else if (mx >= 75 && mx < 135) { current_tool = Tool::Eraser; strcpy(status_msg, "Tool: Eraser"); }
                    else if (mx >= 140 && mx < 190) { current_tool = Tool::Line; strcpy(status_msg, "Tool: Line"); }
                    else if (mx >= 195 && mx < 255) { current_tool = Tool::Rect; strcpy(status_msg, "Tool: Rectangle"); }
                    else if (mx >= 260 && mx < 330) { current_tool = Tool::FillRect; strcpy(status_msg, "Tool: Fill Rect"); }
                    else if (mx >= 335 && mx < 395) { current_tool = Tool::Circle; strcpy(status_msg, "Tool: Circle"); }
                    else if (mx >= 400 && mx < 475) { current_tool = Tool::FillCircle; strcpy(status_msg, "Tool: Fill Circle"); }
                    else if (mx >= 480 && mx < 545) { current_tool = Tool::FloodFill; strcpy(status_msg, "Tool: Bucket Fill"); }
                    else if (mx >= 550 && mx < 620) { current_tool = Tool::Eyedropper; strcpy(status_msg, "Tool: Eyedropper"); }
                }

                else if (my >= static_cast<int>(canvas_h) + 35 && my <= static_cast<int>(canvas_h) + 60) {
                    if (mx >= 10 && mx < 70) {
                        char filename[64] = "drawing.png";
                        if (Kernel::UICommon::prompt_filename(vfb, "Save filename:", filename, sizeof(filename))) {
                            save_png(filename, canvas, canvas_w, canvas_h);
                            strcpy(status_msg, "File saved successfully.");
                        }
                    } else if (mx >= 75 && mx < 135) {
                        char filename[64] = "drawing.png";
                        if (Kernel::UICommon::prompt_filename(vfb, "Open filename:", filename, sizeof(filename))) {
                            if (load_png(filename, canvas, canvas_w, canvas_h)) {
                                memcpy(backup_canvas, canvas, canvas_w * canvas_h * sizeof(uint32_t));
                                strcpy(status_msg, "File loaded successfully.");
                            } else {
                                strcpy(status_msg, "Error loading file.");
                            }
                        }
                    } else if (mx >= 140 && mx < 180) { brush_width = brush_width > 1 ? brush_width - 1 : 1; }
                    else if (mx >= 185 && mx < 225) { brush_width = brush_width < 20 ? brush_width + 1 : 20; }
                    else if (mx >= 230 && mx < 280) { running = false; }
                }

                else if (my >= static_cast<int>(canvas_h) + 65) {
                    int cp_x = 10;
                    if (mx >= cp_x && mx < cp_x + 200 && my >= static_cast<int>(canvas_h) + 65 && my < static_cast<int>(canvas_h) + 90) {
                        float ratio = static_cast<float>(mx - cp_x) / 200.0f;
                        if (ratio < 0.25f) {
                            cur_r = 255; cur_g = static_cast<uint8_t>(ratio * 4.0f * 255.0f); cur_b = 0;
                        } else if (ratio < 0.5f) {
                            cur_r = static_cast<uint8_t>((2.0f - ratio * 4.0f) * 255.0f); cur_g = 255; cur_b = 0;
                        } else if (ratio < 0.75f) {
                            cur_r = 0; cur_g = 255; cur_b = static_cast<uint8_t>((ratio * 4.0f - 2.0f) * 255.0f);
                        } else {
                            cur_r = 0; cur_g = static_cast<uint8_t>((4.0f - ratio * 4.0f) * 255.0f); cur_b = 255;
                        }
                        strcpy(status_msg, "Selected color from fancy wheel.");
                    }
                }
            }
        }

        mouse_down_prev = m_left;

        vfb.clear(COLOR_BG);
        vfb.blit(canvas, 0, 0, canvas_w, canvas_h, canvas_w * sizeof(uint32_t));

        vfb.fill_rect(0, canvas_h, sw, 100, 0xFF313244);
        vfb.draw_rect(0, canvas_h, sw, 100, 0xFF45475A);

        const char* tool_names[] = { "Pencil", "Eraser", "Line", "Rect", "FillR", "Circle", "FillC", "Bucket", "Drop" };
        int btn_x = 10;
        for (int i = 0; i < 9; ++i) {
            bool active = (static_cast<int>(current_tool) == i);
            Kernel::UICommon::draw_button(vfb, btn_x, canvas_h + 5, 55, 24, tool_names[i],
                active ? 0xFF89B4FA : 0xFF45475A,
                active ? 0xFF1E1E2E : COLOR_TEXT,
                0);
            btn_x += 65;
        }

        Kernel::UICommon::draw_button(vfb, 10, canvas_h + 35, 60, 24, "Save", 0xFF45475A, COLOR_TEXT, 0);
        Kernel::UICommon::draw_button(vfb, 75, canvas_h + 35, 60, 24, "Open", 0xFF45475A, COLOR_TEXT, 0);
        Kernel::UICommon::draw_button(vfb, 140, canvas_h + 35, 38, 24, "-", 0xFF45475A, COLOR_TEXT, 0);
        Kernel::UICommon::draw_button(vfb, 185, canvas_h + 38, 38, 24, "+", 0xFF45475A, COLOR_TEXT, 0);
        Kernel::UICommon::draw_button(vfb, 230, canvas_h + 35, 50, 24, "Quit", 0xFFF38BA8, 0xFF1E1E2E, 0);

        char width_buf[32] = "W:";
        int idx = 2;
        if (brush_width >= 10) {
            width_buf[idx++] = '0' + (brush_width / 10);
            width_buf[idx++] = '0' + (brush_width % 10);
        } else {
            width_buf[idx++] = '0' + brush_width;
        }
        width_buf[idx] = '\0';
        Kernel::AppCommon::text(vfb, 290, canvas_h + 41, width_buf, COLOR_TEXT);

        int cp_x = 10;
        for (int i = 0; i < 200; ++i) {
            float ratio = static_cast<float>(i) / 200.0f;
            uint8_t r = 0, g = 0, b = 0;
            if (ratio < 0.25f) {
                r = 255; g = static_cast<uint8_t>(ratio * 4.0f * 255.0f); b = 0;
            } else if (ratio < 0.5f) {
                r = static_cast<uint8_t>((2.0f - ratio * 4.0f) * 255.0f); g = 255; b = 0;
            } else if (ratio < 0.75f) {
                r = 0; g = 255; b = static_cast<uint8_t>((ratio * 4.0f - 2.0f) * 255.0f);
            } else {
                r = 0; g = static_cast<uint8_t>((4.0f - ratio * 4.0f) * 255.0f); b = 255;
            }
            uint32_t col = 0xFF000000 | (r << 16) | (g << 8) | b;
            vfb.fill_rect(cp_x + i, canvas_h + 65, 1, 22, col);
        }
        vfb.draw_rect(cp_x, canvas_h + 65, 200, 22, 0xFFCDD6F4);

        uint32_t current_color = 0xFF000000 | (cur_r << 16) | (cur_g << 8) | cur_b;
        vfb.fill_rect(220, canvas_h + 65, 40, 22, current_color);
        vfb.draw_rect(220, canvas_h + 65, 40, 22, 0xFFCDD6F4);

        Kernel::UICommon::draw_status_bar(vfb, 10, canvas_h + 90, sw - 20, 0, status_msg, 0xFF313244, COLOR_TEXT, 0);
        vfb.draw_rect(mx - 3, my - 3, 7, 7, COLOR_CURSOR);

        vfb.flush();
    }

    kfree(canvas);
    kfree(backup_canvas);
}

}
