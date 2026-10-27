#include <kernel/graphics/window_manager.h>
#include <kernel/graphics/vfb.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/mouse.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>
#include <arch/x86_64/string.h>
#include <arch/x86_64/spinlock.h>

namespace Kernel::Graphics::WindowManager {
namespace {
constexpr size_t MAX_WINDOWS = 8;
constexpr int TITLE_H = 26;
constexpr uint32_t DESKTOP = 0x18232E;
constexpr uint32_t TITLE_ACTIVE = 0x42698A;
constexpr uint32_t TITLE_INACTIVE = 0x344657;
constexpr uint32_t BORDER = 0x8AAFC4;
constexpr uint32_t TEXT = 0xF1F5F8;
constexpr size_t FRAME_BUFFER_COUNT = 3;
constexpr uint8_t INVALID_BUFFER = 0xFF;

struct Window {
    uint64_t thread_id;
    char title[32];
    uint32_t* pixels[FRAME_BUFFER_COUNT];
    uint8_t front_index;
    uint8_t render_index;
    uint8_t write_index;
    uint32_t width, height;
    int x, y;
    bool alive, launcher, has_frame;
};

struct RenderWindow {
    Window* owner;
    uint64_t thread_id;
    char title[32];
    const uint32_t* pixels;
    uint32_t width, height;
    int x, y;
    uint8_t buffer_index;
    bool has_frame, focused;
};

Window windows[MAX_WINDOWS]{};
size_t window_count = 0;
size_t focused = 0;
uint32_t screen_w = 0, screen_h = 0;
VirtualFramebuffer desktop;
int drag_x = 0, drag_y = 0;
bool dragging = false, old_left = false;
int cursor_x = -1, cursor_y = -1;
Spinlock compositor_lock;
bool damage_valid = false;
int damage_left = 0, damage_top = 0, damage_right = 0, damage_bottom = 0;

void copy_name(char* dst, const char* src) {
    size_t i = 0;
    if (src) for (; i + 1 < 32 && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

void draw_text(int x, int y, const char* s, uint32_t color) {
    while (*s) {
        desktop.draw_glyph(x, y, *s++, color);
        x += 8;
    }
}

void mark_dirty(int left, int top, int right, int bottom) {
    if (left >= right || top >= bottom) return;
    if (damage_valid) {
        if (left < damage_left) damage_left = left;
        if (top < damage_top) damage_top = top;
        if (right > damage_right) damage_right = right;
        if (bottom > damage_bottom) damage_bottom = bottom;
        return;
    }
    damage_valid = true;
    damage_left = left;
    damage_top = top;
    damage_right = right;
    damage_bottom = bottom;
}

void mark_window_dirty(const Window& w) {
    mark_dirty(w.x - 1, w.y - 1,
               w.x + static_cast<int>(w.width) + 1,
               w.y + TITLE_H + static_cast<int>(w.height) + 1);
}

void mark_full_dirty() {
    mark_dirty(0, 0, static_cast<int>(screen_w), static_cast<int>(screen_h));
}

void fill_rect_clipped(int x, int y, int width, int height, uint32_t color,
                       int left, int top, int right, int bottom) {
    const int clipped_left = x > left ? x : left;
    const int clipped_top = y > top ? y : top;
    const int clipped_right = (x + width) < right ? x + width : right;
    const int clipped_bottom = (y + height) < bottom ? y + height : bottom;
    if (clipped_left < clipped_right && clipped_top < clipped_bottom)
        desktop.fill_rect(clipped_left, clipped_top,
                          clipped_right - clipped_left, clipped_bottom - clipped_top, color);
}

void blit_desktop_region(int left, int top, int right, int bottom) {
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > static_cast<int>(screen_w)) right = static_cast<int>(screen_w);
    if (bottom > static_cast<int>(screen_h)) bottom = static_cast<int>(screen_h);
    if (left >= right || top >= bottom) return;

    const uint32_t width = static_cast<uint32_t>(right - left);
    const uint32_t height = static_cast<uint32_t>(bottom - top);
    const uint32_t* source = desktop.buffer() + static_cast<size_t>(top) * screen_w + left;
    Kernel::Drivers::Framebuffer::blit(source, static_cast<uint32_t>(left),
                                       static_cast<uint32_t>(top), width, height,
                                       desktop.pitch());
}

void render_region(const RenderWindow* rendered_windows, size_t rendered_count,
                   int left, int top, int right, int bottom) {
    if (!desktop.valid()) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > static_cast<int>(screen_w)) right = static_cast<int>(screen_w);
    if (bottom > static_cast<int>(screen_h)) bottom = static_cast<int>(screen_h);
    if (left >= right || top >= bottom) return;

    desktop.fill_rect(left, top, right - left, bottom - top, DESKTOP);
    fill_rect_clipped(0, 0, static_cast<int>(screen_w), 30, 0x22313E,
                      left, top, right, bottom);
    draw_text(12, 10, "OSFOS DESKTOP", TEXT);

    for (size_t i = 0; i < rendered_count; ++i) {
        const RenderWindow& w = rendered_windows[i];
        const int outer_w = static_cast<int>(w.width) + 2;
        const int outer_h = static_cast<int>(w.height) + TITLE_H + 2;
        const uint32_t title_color = w.focused ? TITLE_ACTIVE : TITLE_INACTIVE;
        fill_rect_clipped(w.x - 1, w.y - 1, outer_w, outer_h, BORDER,
                          left, top, right, bottom);
        fill_rect_clipped(w.x, w.y, static_cast<int>(w.width), TITLE_H, title_color,
                          left, top, right, bottom);
        draw_text(w.x + 9, w.y + 8, w.title, TEXT);
        fill_rect_clipped(w.x + static_cast<int>(w.width) - 22, w.y + 4, 16, 16, 0x9E4D55,
                          left, top, right, bottom);
        desktop.draw_glyph(w.x + static_cast<int>(w.width) - 18, w.y + 8, 'X', TEXT);
        fill_rect_clipped(w.x, w.y + TITLE_H, static_cast<int>(w.width), static_cast<int>(w.height),
                          0x101820, left, top, right, bottom);
        if (w.has_frame && w.pixels) {
            const int body_top = w.y + TITLE_H;
            const int copy_left = w.x > left ? w.x : left;
            const int copy_top = body_top > top ? body_top : top;
            const int copy_right = (w.x + static_cast<int>(w.width)) < right
                ? w.x + static_cast<int>(w.width) : right;
            const int copy_bottom = (body_top + static_cast<int>(w.height)) < bottom
                ? body_top + static_cast<int>(w.height) : bottom;
            if (copy_left < copy_right && copy_top < copy_bottom) {
                const uint32_t* source = w.pixels +
                    static_cast<size_t>(copy_top - body_top) * w.width +
                    static_cast<size_t>(copy_left - w.x);
                desktop.blit(source, static_cast<uint32_t>(copy_left),
                             static_cast<uint32_t>(copy_top),
                             static_cast<uint32_t>(copy_right - copy_left),
                             static_cast<uint32_t>(copy_bottom - copy_top),
                             w.width * sizeof(uint32_t));
            }
        }
    }

    const int mx = cursor_x;
    const int my = cursor_y;
    fill_rect_clipped(mx - 1, my - 7, 3, 15, TEXT, left, top, right, bottom);
    fill_rect_clipped(mx - 7, my - 1, 15, 3, TEXT, left, top, right, bottom);
    blit_desktop_region(left, top, right, bottom);
}

bool mouse_update() {
    const bool left = Kernel::Drivers::Mouse::raw_left_pressed();
    const int mx = Kernel::Drivers::Mouse::get_x();
    const int my = Kernel::Drivers::Mouse::get_y();
    bool changed = left != old_left || mx != cursor_x || my != cursor_y;
    const int previous_cursor_x = cursor_x;
    const int previous_cursor_y = cursor_y;
    cursor_x = mx;
    cursor_y = my;
    if (left && !old_left) {
        for (size_t n = window_count; n > 0; --n) {
            const size_t i = n - 1;
            Window& w = windows[i];
            if (!w.alive) continue;
            const int ow = static_cast<int>(w.width) + 2;
            const int oh = static_cast<int>(w.height) + TITLE_H + 2;
            if (mx < w.x - 1 || mx >= w.x + ow || my < w.y - 1 || my >= w.y + oh) continue;
            if (focused < window_count && windows[focused].alive)
                mark_window_dirty(windows[focused]);
            focused = i;
            if (i + 1 != window_count) {
                Window selected = w;
                for (size_t j = i; j + 1 < window_count; ++j) windows[j] = windows[j + 1];
                windows[window_count - 1] = selected;
                focused = window_count - 1;
            }
            Window& selected = windows[focused];
            mark_window_dirty(selected);
            if (my >= selected.y && my < selected.y + TITLE_H &&
                mx >= selected.x + static_cast<int>(selected.width) - 24) {
                selected.alive = false;
                Kernel::Scheduler::terminate_thread(selected.thread_id);
            } else if (my < selected.y + TITLE_H) {
                dragging = true;
                drag_x = mx - selected.x;
                drag_y = my - selected.y;
            }
            break;
        }
    }
    if (left && dragging && focused < window_count) {
        Window& w = windows[focused];
        const int previous_x = w.x;
        const int previous_y = w.y;
        w.x = mx - drag_x;
        w.y = my - drag_y;
        if (w.x < 0) w.x = 0;
        if (w.y < 30) w.y = 30;
        if (w.x + static_cast<int>(w.width) > static_cast<int>(screen_w)) w.x = screen_w - w.width;
        if (w.y + static_cast<int>(w.height) + TITLE_H > static_cast<int>(screen_h)) w.y = screen_h - w.height - TITLE_H;
        mark_dirty(previous_x - 1, previous_y - 1,
                   previous_x + static_cast<int>(w.width) + 1,
                   previous_y + TITLE_H + static_cast<int>(w.height) + 1);
        mark_window_dirty(w);
    }
    if (!left) dragging = false;
    old_left = left;
    if (changed) {
        mark_dirty(previous_cursor_x - 7, previous_cursor_y - 7,
                   previous_cursor_x + 8, previous_cursor_y + 8);
        mark_dirty(mx - 7, my - 7, mx + 8, my + 8);
    }
    return changed;
}

bool take_damage_locked(RenderWindow* rendered_windows, size_t& rendered_count,
                        int& left, int& top, int& right, int& bottom) {
    if (!damage_valid) return false;
    left = damage_left;
    top = damage_top;
    right = damage_right;
    bottom = damage_bottom;
    rendered_count = 0;
    for (size_t i = 0; i < window_count; ++i) {
        Window& w = windows[i];
        if (!w.alive) continue;
        RenderWindow& snapshot = rendered_windows[rendered_count++];
        snapshot.owner = &w;
        snapshot.thread_id = w.thread_id;
        copy_name(snapshot.title, w.title);
        snapshot.pixels = w.has_frame ? w.pixels[w.front_index] : nullptr;
        snapshot.width = w.width;
        snapshot.height = w.height;
        snapshot.x = w.x;
        snapshot.y = w.y;
        snapshot.buffer_index = w.front_index;
        snapshot.has_frame = w.has_frame;
        snapshot.focused = (i == focused);
        w.render_index = w.front_index;
    }
    damage_valid = false;
    return true;
}

void release_render_buffers(const RenderWindow* rendered_windows, size_t rendered_count) {
    SpinlockGuard guard(compositor_lock);
    for (size_t i = 0; i < rendered_count; ++i) {
        const RenderWindow& snapshot = rendered_windows[i];
        Window& w = *snapshot.owner;
        if (w.thread_id == snapshot.thread_id &&
            w.render_index == snapshot.buffer_index) {
            w.render_index = INVALID_BUFFER;
        }
    }
}
}

bool initialize() {
    if (!Kernel::Drivers::Framebuffer::valid()) return false;
    screen_w = Kernel::Drivers::Framebuffer::width();
    screen_h = Kernel::Drivers::Framebuffer::height();
    if (screen_w < 240 || screen_h < 160) return false;
    return desktop.init(0, 0, screen_w, screen_h);
}

int register_window(uint64_t thread_id, const char* title, bool launcher,
                    uint32_t width, uint32_t height) {
    SpinlockGuard guard(compositor_lock);
    if (!desktop.valid() || !thread_id) return -1;
    size_t slot = window_count;
    for (size_t i = 0; i < window_count; ++i)
        if (windows[i].thread_id == thread_id && windows[i].alive) return static_cast<int>(i);
        else if (!windows[i].alive && slot == window_count) slot = i;
    if (slot == window_count && window_count == MAX_WINDOWS) return -1;

    const uint32_t margin = launcher ? 24 : 0;
    if (!width) width = launcher ? (screen_w > 48 ? screen_w - 48 : screen_w) : 320;
    if (!height) height = launcher ? (screen_h > 100 ? screen_h - 100 : screen_h - TITLE_H - 2) : 200;
    const uint32_t max_width = screen_w > 2 ? screen_w - 2 : screen_w;
    const uint32_t max_height = screen_h > TITLE_H + 2 ? screen_h - TITLE_H - 2 : screen_h;
    if (width > max_width) width = max_width;
    if (height > max_height) height = max_height;
    Window recycled = windows[slot];
    uint32_t* frames[FRAME_BUFFER_COUNT]{};
    const bool can_reuse = slot < window_count && recycled.width == width &&
                           recycled.height == height &&
                           recycled.render_index == INVALID_BUFFER &&
                           recycled.write_index == INVALID_BUFFER;
    if (can_reuse) {
        for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
            frames[i] = recycled.pixels[i];
    } else {
        const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint32_t);
        for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i) {
            frames[i] = static_cast<uint32_t*>(kmalloc(bytes));
            if (!frames[i]) {
                for (size_t j = 0; j < i; ++j) kfree(frames[j]);
                return -1;
            }
        }
    }
    const size_t frame_bytes = static_cast<size_t>(width) * height * sizeof(uint32_t);
    for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
        memset(frames[i], 0, frame_bytes);

    if (slot < window_count && slot + 1 < window_count) {
        Window recycled = windows[slot];
        for (size_t i = slot; i + 1 < window_count; ++i) windows[i] = windows[i + 1];
        windows[window_count - 1] = recycled;
        slot = window_count - 1;
    }
    Window& w = windows[slot];
    w.thread_id = thread_id;
    copy_name(w.title, title);
    for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
        w.pixels[i] = frames[i];
    w.front_index = 0;
    w.render_index = INVALID_BUFFER;
    w.write_index = INVALID_BUFFER;
    w.width = width;
    w.height = height;
    const int offset = static_cast<int>(slot * 26);
    w.x = margin + offset;
    w.y = (launcher ? 42 : 48) + offset;
    if (w.x + static_cast<int>(width) > static_cast<int>(screen_w)) w.x = 0;
    if (w.y + static_cast<int>(height) + TITLE_H > static_cast<int>(screen_h)) w.y = 30;
    w.alive = true;
    w.launcher = launcher;
    w.has_frame = false;
    if (slot == window_count) ++window_count;
    focused = slot;
    mark_window_dirty(w);
    return static_cast<int>(focused);
}

bool request_window_size(uint64_t thread_id, uint32_t requested_width,
                         uint32_t requested_height, uint32_t* actual_width,
                         uint32_t* actual_height) {
    if (!thread_id || !requested_width || !requested_height || !actual_width || !actual_height) return false;
    SpinlockGuard guard(compositor_lock);
    Window* window = nullptr;
    for (size_t i = 0; i < window_count; ++i)
        if (windows[i].thread_id == thread_id && windows[i].alive) { window = &windows[i]; break; }
    if (!window) return false;

    uint32_t width = requested_width;
    uint32_t height = requested_height;
    const uint32_t max_width = screen_w > 2 ? screen_w - 2 : screen_w;
    const uint32_t max_height = screen_h > TITLE_H + 32 ? screen_h - TITLE_H - 32 : screen_h;
    if (width > max_width) width = max_width;
    if (height > max_height) height = max_height;
    if (window->width == width && window->height == height) {
        *actual_width = width;
        *actual_height = height;
        return true;
    }

    if (window->render_index != INVALID_BUFFER || window->write_index != INVALID_BUFFER) {
        *actual_width = window->width;
        *actual_height = window->height;
        return true;
    }

    uint32_t* frames[FRAME_BUFFER_COUNT]{};
    while (width >= 120 && height >= 80) {
        size_t allocated = 0;
        const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint32_t);
        for (; allocated < FRAME_BUFFER_COUNT; ++allocated) {
            frames[allocated] = static_cast<uint32_t*>(kmalloc(bytes));
            if (!frames[allocated]) break;
        }
        if (allocated == FRAME_BUFFER_COUNT) break;
        while (allocated) {
            --allocated;
            kfree(frames[allocated]);
            frames[allocated] = nullptr;
        }
        if (width >= height && width > 120) width = width * 3 / 4;
        else if (height > 80) height = height * 3 / 4;
        else break;
    }
    if (!frames[0]) {
        *actual_width = window->width;
        *actual_height = window->height;
        return true;
    }

    mark_window_dirty(*window);
    for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        kfree(window->pixels[i]);
        window->pixels[i] = frames[i];
        memset(frames[i], 0, static_cast<size_t>(width) * height * sizeof(uint32_t));
    }
    window->width = width;
    window->height = height;
    window->front_index = 0;
    window->has_frame = false;
    window->x = static_cast<int>((screen_w - width) / 2);
    window->y = 30 + static_cast<int>((screen_h - 30 - TITLE_H - height) / 2);
    mark_window_dirty(*window);
    *actual_width = width;
    *actual_height = height;
    return true;
}

void unregister_window(uint64_t thread_id) {
    SpinlockGuard guard(compositor_lock);
    for (size_t i = 0; i < window_count; ++i) {
        if (windows[i].thread_id != thread_id || !windows[i].alive) continue;
        mark_window_dirty(windows[i]);
        windows[i].alive = false;
        if (focused == i) {
            focused = 0;
            for (size_t j = window_count; j > 0; --j) {
                if (windows[j - 1].alive) { focused = j - 1; break; }
            }
        }
        return;
    }
}

bool present(uint64_t thread_id, const uint32_t* pixels, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!pixels || !width || !height) return false;

    uint32_t* destination = nullptr;
    uint32_t destination_width = 0;
    int write_index = -1;
    {
        SpinlockGuard guard(compositor_lock);
        for (size_t i = 0; i < window_count; ++i) {
            Window& w = windows[i];
            if (w.thread_id != thread_id) continue;
            if (!w.alive) return true;
            if (width != w.width || height != w.height || pitch < width * sizeof(uint32_t)) return true;
            for (size_t buffer = 0; buffer < FRAME_BUFFER_COUNT; ++buffer) {
                if (buffer != w.front_index && buffer != w.render_index &&
                    buffer != w.write_index) {
                    write_index = static_cast<int>(buffer);
                    break;
                }
            }
            // A renderer may temporarily pin two buffers. Drop this frame
            // rather than falling back to direct framebuffer writes.
            if (write_index < 0) return true;
            destination = w.pixels[write_index];
            destination_width = w.width;
            w.write_index = static_cast<uint8_t>(write_index);
            break;
        }
    }
    if (write_index < 0 || !destination) return false;

    memcpy(destination, pixels, static_cast<size_t>(destination_width) * height * sizeof(uint32_t));

    SpinlockGuard guard(compositor_lock);
    for (size_t i = 0; i < window_count; ++i) {
        Window& w = windows[i];
        if (w.thread_id != thread_id || w.write_index != write_index ||
            w.pixels[write_index] != destination)
            continue;
        w.front_index = static_cast<uint8_t>(write_index);
        w.write_index = INVALID_BUFFER;
        w.has_frame = true;
        mark_window_dirty(w);
        return true;
    }
    return true;
}

bool has_window(uint64_t thread_id) {
    SpinlockGuard guard(compositor_lock);
    for (size_t i = 0; i < window_count; ++i)
        if (windows[i].thread_id == thread_id && windows[i].alive) return true;
    return false;
}

bool title_is_open(const char* title) {
    SpinlockGuard guard(compositor_lock);
    if (!title) return false;
    for (size_t i = 0; i < window_count; ++i) {
        if (!windows[i].alive) continue;
        size_t j = 0;
        while (title[j] && windows[i].title[j] && title[j] == windows[i].title[j]) ++j;
        if (!title[j] && !windows[i].title[j]) return true;
    }
    return false;
}

bool has_keyboard_focus(uint64_t thread_id) {
    SpinlockGuard guard(compositor_lock);
    return focused < window_count && windows[focused].alive && windows[focused].thread_id == thread_id;
}

void run() {
    RenderWindow rendered_windows[MAX_WINDOWS]{};
    for (;;) {
        size_t rendered_count = 0;
        int left = 0, top = 0, right = 0, bottom = 0;
        {
            SpinlockGuard guard(compositor_lock);
            if (cursor_x < 0) {
                cursor_x = Kernel::Drivers::Mouse::get_x();
                cursor_y = Kernel::Drivers::Mouse::get_y();
                mark_full_dirty();
            }
            mouse_update();
            if (take_damage_locked(rendered_windows, rendered_count,
                                   left, top, right, bottom)) {
                // Rendering happens after the lock is released. The selected
                // frame buffers are pinned until release_render_buffers().
            }
        }
        if (rendered_count) {
            render_region(rendered_windows, rendered_count, left, top, right, bottom);
            release_render_buffers(rendered_windows, rendered_count);
        }
        Kernel::Scheduler::thread_sleep_ms(4);
    }
}
}
