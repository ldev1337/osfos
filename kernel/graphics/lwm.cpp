#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <arch/x86_64/spinlock.h>
#include <arch/x86_64/string.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/mouse.h>
#include <kernel/graphics/simplefonts.h>
#include <kernel/graphics/vfb.h>
#include <kernel/graphics/window_manager.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>

namespace Kernel::Graphics::WindowManager {
namespace {

constexpr size_t MAX_WINDOWS = 32;
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
  std::array<uint32_t *, FRAME_BUFFER_COUNT> pixels{};
  uint8_t front_index;
  uint8_t render_index;
  uint8_t write_index;
  uint32_t width, height;
  int x, y;
  bool alive, has_frame;
};

struct RenderWindow {
  Window *owner;
  uint64_t thread_id;
  const char *title;
  const uint32_t *pixels;
  uint32_t width, height;
  int x, y;
  uint8_t buffer_index;
  bool focused;
};

std::array<Window, MAX_WINDOWS> windows{};
size_t window_count = 0;
std::array<size_t, MAX_WINDOWS> z_order{};
size_t focused_index_in_z = 0;

uint32_t screen_w = 0, screen_h = 0;
int drag_x = 0, drag_y = 0;
bool dragging = false, old_left = false;
int cursor_x = -1, cursor_y = -1;
Spinlock compositor_lock;
bool damage_valid = false;
int damage_left = 0, damage_top = 0, damage_right = 0, damage_bottom = 0;

void copy_name(char *dst, const char *src) {
  size_t i = 0;
  if (src)
    for (; i + 1 < 32 && src[i]; ++i)
      dst[i] = src[i];
  dst[i] = '\0';
}

void draw_glyph(VirtualFramebuffer &fb, int gx, int gy, char ch, uint32_t color,
                int cl, int ct, int cr, int cb) {
  if (gx + 8 <= cl || gx >= cr || gy + 8 <= ct || gy >= cb)
    return;
  const auto c = static_cast<unsigned char>(ch);
  const auto *bits =
      Kernel::Graphics::Font::Basic8x8[(c >= 32 && c <= 127) ? c - 32
                                                             : '?' - 32];
  auto *buf = fb.buffer();
  const int fb_w = fb.width();
  const int y_start = std::max(0, ct - gy);
  const int y_end = std::min(8, cb - gy);
  const int x_start = std::max(0, cl - gx);
  const int x_end = std::min(8, cr - gx);

  for (int py = y_start; py < y_end; ++py) {
    const uint8_t row = bits[py];
    const int screen_y = gy + py;
    auto *dst = buf + static_cast<size_t>(screen_y - ct) * fb_w;
    for (int px = x_start; px < x_end; ++px) {
      if (row & (1u << px)) {
        const int screen_x = gx + px;
        dst[screen_x - cl] = color;
      }
    }
  }
}

void draw_text(VirtualFramebuffer &fb, int x, int y, const char *s,
               uint32_t color, int cl, int ct, int cr, int cb) {
  if (!s || !fb.valid())
    return;
  for (; *s && x < cr; x += 8, ++s) {
    if (x + 8 > cl)
      draw_glyph(fb, x, y, *s, color, cl, ct, cr, cb);
  }
}

void free_window_pixels(Window &w) {
  for (auto *&p : w.pixels) {
    if (p) {
      kfree(p);
      p = nullptr;
    }
  }
}

bool allocate_frames(uint32_t width, uint32_t height,
                     std::array<uint32_t *, FRAME_BUFFER_COUNT> &frames) {
  const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint32_t);
  for (auto *&f : frames) {
    f = static_cast<uint32_t *>(kmalloc(bytes));
    if (!f) {
      for (auto *&c : frames) {
        if (c) {
          kfree(c);
          c = nullptr;
        }
      }
      return false;
    }
  }
  return true;
}

void mark_dirty(int left, int top, int right, int bottom) {
  if (left >= right || top >= bottom)
    return;
  if (!damage_valid) {
    damage_left = left;
    damage_top = top;
    damage_right = right;
    damage_bottom = bottom;
    damage_valid = true;
  } else {
    damage_left = std::min(damage_left, left);
    damage_top = std::min(damage_top, top);
    damage_right = std::max(damage_right, right);
    damage_bottom = std::max(damage_bottom, bottom);
  }
}

void mark_window_dirty(const Window &w) {
  mark_dirty(w.x - 1, w.y - 1, w.x + static_cast<int>(w.width) + 1,
             w.y + TITLE_H + static_cast<int>(w.height) + 1);
}

void fill_rect_clipped(VirtualFramebuffer &fb, int x, int y, int width,
                       int height, uint32_t color, int left, int top, int right,
                       int bottom) {
  const int cl = std::max(x, left);
  const int ct = std::max(y, top);
  const int cr = std::min(x + width, right);
  const int cb = std::min(y + height, bottom);
  if (cl < cr && ct < cb)
    fb.fill_rect(cl - left, ct - top, cr - cl, cb - ct, color);
}

bool clip_to_screen(int &l, int &t, int &r, int &b) {
  l = std::max(0, l);
  t = std::max(0, t);
  r = std::min(static_cast<int>(screen_w), r);
  b = std::min(static_cast<int>(screen_h), b);
  return l < r && t < b;
}

void bring_to_front(size_t w_idx) {
  if (window_count == 0)
    return;
  size_t pos = window_count;
  for (size_t i = 0; i < window_count; ++i) {
    if (z_order[i] == w_idx) {
      pos = i;
      break;
    }
  }
  if (pos == window_count || pos == window_count - 1) {
    if (pos == window_count - 1)
      focused_index_in_z = pos;
    return;
  }
  for (size_t i = pos; i + 1 < window_count; ++i)
    z_order[i] = z_order[i + 1];
  z_order[window_count - 1] = w_idx;
  focused_index_in_z = window_count - 1;
}

void remove_from_z_order(size_t window_index) {
  size_t pos = window_count;
  for (size_t i = 0; i < window_count; ++i) {
    if (z_order[i] == window_index) {
      pos = i;
      break;
    }
  }
  if (pos == window_count)
    return;
  for (size_t i = pos; i + 1 < window_count; ++i)
    z_order[i] = z_order[i + 1];
  focused_index_in_z = --window_count ? window_count - 1 : 0;
}

Window *find_window(uint64_t thread_id) {
  if (!thread_id)
    return nullptr;
  for (auto &w : windows) {
    if (w.alive && w.thread_id == thread_id)
      return &w;
  }
  return nullptr;
}

void unregister_window_index(size_t i) {
  if (i >= MAX_WINDOWS || !windows[i].alive)
    return;
  auto &w = windows[i];
  mark_window_dirty(w);
  w.alive = false;
  if (w.render_index == INVALID_BUFFER && w.write_index == INVALID_BUFFER)
    free_window_pixels(w);
  remove_from_z_order(i);
}

int register_window_locked(uint64_t thread_id, const char *title, bool launcher,
                           uint32_t width, uint32_t height) {
  if (!Kernel::Drivers::Framebuffer::valid() || !thread_id)
    return -1;

  size_t slot = MAX_WINDOWS;
  for (size_t i = 0; i < MAX_WINDOWS; ++i) {
    if (windows[i].alive) {
      if (windows[i].thread_id == thread_id) {
        bring_to_front(i);
        return static_cast<int>(focused_index_in_z);
      }
    } else if (slot == MAX_WINDOWS) {
      slot = i;
    }
  }

  // Never refuse an app: evict bottom-most window if at limit
  if (slot == MAX_WINDOWS) {
    if (window_count > 0) {
      slot = z_order[0];
      const uint64_t evict_tid = windows[slot].thread_id;
      unregister_window_index(slot);
      Kernel::Scheduler::terminate_thread(evict_tid);
    } else {
      slot = 0;
    }
  }

  const uint32_t margin = launcher ? 24 : 0;
  if (width == 0U)
    width = launcher ? (screen_w > 48 ? screen_w - 48 : screen_w) : 320;
  if (height == 0U)
    height = launcher
                 ? (screen_h > 100 ? screen_h - 100 : screen_h - TITLE_H - 2)
                 : 200;

  std::array<uint32_t *, FRAME_BUFFER_COUNT> frames{};
  auto &recycled = windows[slot];
  const bool can_reuse = recycled.width == width && recycled.height == height &&
                         (recycled.pixels[0] != nullptr) &&
                         recycled.render_index == INVALID_BUFFER &&
                         recycled.write_index == INVALID_BUFFER;
  if (can_reuse) {
    frames = recycled.pixels;
  } else {
    free_window_pixels(recycled);
    if (!allocate_frames(width, height, frames))
      return -1;
  }

  auto &w = windows[slot];
  w.thread_id = thread_id;
  copy_name(w.title, title);
  w.pixels = frames;
  w.front_index = 0;
  w.render_index = w.write_index = INVALID_BUFFER;
  w.width = width;
  w.height = height;

  const int offset = static_cast<int>((slot % 8) * 26);
  w.x =
      (margin + offset + static_cast<int>(width) <= static_cast<int>(screen_w))
          ? (margin + offset)
          : 0;
  w.y = (launcher ? 42 : 48) + offset;
  if (w.y + static_cast<int>(height) + TITLE_H > static_cast<int>(screen_h))
    w.y = 30;
  w.alive = true;
  w.has_frame = false;

  z_order[window_count++] = slot;
  bring_to_front(slot);
  mark_window_dirty(w);
  return static_cast<int>(focused_index_in_z);
}

void render_region(const RenderWindow *rendered_windows, size_t rendered_count,
                   int left, int top, int right, int bottom) {
  if (!clip_to_screen(left, top, right, bottom))
    return;

  const uint32_t w_w = static_cast<uint32_t>(right - left);
  const uint32_t w_h = static_cast<uint32_t>(bottom - top);
  VirtualFramebuffer region_fb;
  if (!region_fb.init(left, top, w_w, w_h))
    return;

  region_fb.fill_rect(0, 0, w_w, w_h, DESKTOP);

  if (top < 30) {
    fill_rect_clipped(region_fb, 0, 0, screen_w, 30, 0x22313E, left, top, right,
                      bottom);
    if (left < 150)
      draw_text(region_fb, 12, 10, "OSFOS DESKTOP", TEXT, left, top, right,
                bottom);
  }

  for (size_t i = 0; i < rendered_count; ++i) {
    const auto &w = rendered_windows[i];
    const int outer_w = static_cast<int>(w.width) + 2;
    const int outer_h = static_cast<int>(w.height) + TITLE_H + 2;
    const int wl = w.x - 1, wt = w.y - 1;

    // Early window culling against dirty rectangle
    if (wl + outer_w <= left || wl >= right || wt + outer_h <= top ||
        wt >= bottom)
      continue;

    const uint32_t title_color = w.focused ? TITLE_ACTIVE : TITLE_INACTIVE;
    fill_rect_clipped(region_fb, wl, wt, outer_w, outer_h, BORDER, left, top,
                      right, bottom);
    fill_rect_clipped(region_fb, w.x, w.y, w.width, TITLE_H, title_color, left,
                      top, right, bottom);
    draw_text(region_fb, w.x + 9, w.y + 8, w.title, TEXT, left, top,
              std::min(w.x + static_cast<int>(w.width) - 24, right), bottom);

    const int btn_x = w.x + static_cast<int>(w.width) - 22;
    const int btn_y = w.y + 4;
    fill_rect_clipped(region_fb, btn_x, btn_y, 16, 16, 0x9E4D55, left, top,
                      right, bottom);
    draw_glyph(region_fb, btn_x + 4, btn_y + 4, 'X', TEXT, left, top, right,
               bottom);

    const int body_top = w.y + TITLE_H;
    fill_rect_clipped(region_fb, w.x, body_top, w.width, w.height, 0x101820,
                      left, top, right, bottom);

    if (w.pixels) {
      const int cl = std::max(w.x, left);
      const int ct = std::max(body_top, top);
      const int cr = std::min(w.x + static_cast<int>(w.width), right);
      const int cb = std::min(body_top + static_cast<int>(w.height), bottom);
      if (cl < cr && ct < cb) {
        const auto *src = w.pixels +
                          static_cast<size_t>(ct - body_top) * w.width +
                          (cl - w.x);
        region_fb.blit(src, cl - left, ct - top, cr - cl, cb - ct,
                       w.width * sizeof(uint32_t));
      }
    }
  }

  fill_rect_clipped(region_fb, cursor_x - 1, cursor_y - 7, 3, 15, TEXT, left,
                    top, right, bottom);
  fill_rect_clipped(region_fb, cursor_x - 7, cursor_y - 1, 15, 3, TEXT, left,
                    top, right, bottom);

  Kernel::Drivers::Framebuffer::blit(region_fb.buffer(), left, top, w_w, w_h,
                                     region_fb.pitch());
}

void mouse_update() {
  const bool left = Kernel::Drivers::Mouse::raw_left_pressed();
  const bool left_changed = left != old_left;
  const int mx = Kernel::Drivers::Mouse::get_x();
  const int my = Kernel::Drivers::Mouse::get_y();
  const int prev_mx = cursor_x;
  const int prev_my = cursor_y;
  cursor_x = mx;
  cursor_y = my;

  if (left && !old_left) {
    for (size_t n = window_count; n > 0; --n) {
      const size_t i = z_order[n - 1];
      auto &w = windows[i];
      if (!w.alive)
        continue;
      if (mx < w.x - 1 || mx >= w.x + static_cast<int>(w.width) + 1 ||
          my < w.y - 1 || my >= w.y + TITLE_H + static_cast<int>(w.height) + 1)
        continue;

      if (window_count > 0 && windows[z_order[focused_index_in_z]].alive)
        mark_window_dirty(windows[z_order[focused_index_in_z]]);

      bring_to_front(i);
      mark_window_dirty(w);

      if (my >= w.y && my < w.y + TITLE_H &&
          mx >= w.x + static_cast<int>(w.width) - 24) {
        const uint64_t tid = w.thread_id;
        unregister_window_index(i);
        Kernel::Scheduler::terminate_thread(tid);
      } else if (my < w.y + TITLE_H) {
        dragging = true;
        drag_x = mx - w.x;
        drag_y = my - w.y;
      }
      break;
    }
  }

  if (left && dragging && window_count > 0) {
    auto &w = windows[z_order[focused_index_in_z]];
    const int prev_x = w.x;
    const int prev_y = w.y;
    const int new_x = std::clamp(mx - drag_x, -static_cast<int>(w.width) + 32,
                                 static_cast<int>(screen_w) - 32);
    const int new_y =
        std::clamp(my - drag_y, 0, static_cast<int>(screen_h) - TITLE_H);

    if (new_x != prev_x || new_y != prev_y) {
      w.x = new_x;
      w.y = new_y;
      mark_dirty(prev_x - 1, prev_y - 1, prev_x + static_cast<int>(w.width) + 1,
                 prev_y + TITLE_H + static_cast<int>(w.height) + 1);
      mark_window_dirty(w);
    }
  }

  if (!left)
    dragging = false;
  old_left = left;
  if (left_changed || mx != prev_mx || my != prev_my) {
    mark_dirty(prev_mx - 7, prev_my - 7, prev_mx + 8, prev_my + 8);
    mark_dirty(mx - 7, my - 7, mx + 8, my + 8);
  }
}

bool take_damage_locked(RenderWindow *rendered_windows, size_t &rendered_count,
                        int &left, int &top, int &right, int &bottom) {
  if (!damage_valid)
    return false;

  left = std::clamp(damage_left, 0, static_cast<int>(screen_w));
  top = std::clamp(damage_top, 0, static_cast<int>(screen_h));
  right = std::clamp(damage_right, 0, static_cast<int>(screen_w));
  bottom = std::clamp(damage_bottom, 0, static_cast<int>(screen_h));
  damage_valid = false;

  if (left >= right || top >= bottom)
    return false;

  rendered_count = 0;
  for (size_t z = 0; z < window_count; ++z) {
    auto &w = windows[z_order[z]];
    if (!w.alive)
      continue;
    auto &s = rendered_windows[rendered_count++];
    s.owner = &w;
    s.thread_id = w.thread_id;
    s.title = w.title;
    s.pixels = w.has_frame ? w.pixels[w.front_index] : nullptr;
    s.width = w.width;
    s.height = w.height;
    s.x = w.x;
    s.y = w.y;
    s.buffer_index = w.front_index;
    s.focused = (z == focused_index_in_z);
    w.render_index = w.front_index;
  }
  return true;
}

void release_render_buffers(const RenderWindow *rendered_windows,
                            size_t rendered_count) {
  SpinlockGuard guard(compositor_lock);
  for (size_t i = 0; i < rendered_count; ++i) {
    const auto &s = rendered_windows[i];
    auto &w = *s.owner;
    if (w.thread_id == s.thread_id && w.render_index == s.buffer_index)
      w.render_index = INVALID_BUFFER;
    if (!w.alive && w.render_index == INVALID_BUFFER &&
        w.write_index == INVALID_BUFFER)
      free_window_pixels(w);
  }
}

} // namespace

bool initialize() {
  if (!Kernel::Drivers::Framebuffer::valid())
    return false;
  screen_w = Kernel::Drivers::Framebuffer::width();
  screen_h = Kernel::Drivers::Framebuffer::height();
  return (screen_w >= 240 && screen_h >= 160);
}

int register_window(uint64_t thread_id, const char *title, bool launcher,
                    uint32_t width, uint32_t height) {
  SpinlockGuard guard(compositor_lock);
  return register_window_locked(thread_id, title, launcher, width, height);
}

bool request_window_size(uint64_t thread_id, uint32_t requested_width,
                         uint32_t requested_height, uint32_t *actual_width,
                         uint32_t *actual_height) {
  if (!thread_id || !requested_width || !requested_height || !actual_width ||
      !actual_height)
    return false;

  SpinlockGuard guard(compositor_lock);
  auto *window = find_window(thread_id);
  if (!window) {
    if (register_window_locked(thread_id, "App", false, requested_width,
                               requested_height) < 0)
      return false;
    window = find_window(thread_id);
    if (!window)
      return false;
    *actual_width = window->width;
    *actual_height = window->height;
    return true;
  }

  const uint32_t width = requested_width;
  const uint32_t height = requested_height;
  if (window->width == width && window->height == height) {
    *actual_width = width;
    *actual_height = height;
    return true;
  }

  std::array<uint32_t *, FRAME_BUFFER_COUNT> frames{};
  if (!allocate_frames(width, height, frames)) {
    *actual_width = window->width;
    *actual_height = window->height;
    return true;
  }

  mark_window_dirty(*window);
  free_window_pixels(*window);
  window->pixels = frames;
  window->width = width;
  window->height = height;
  window->front_index = 0;
  window->render_index = INVALID_BUFFER;
  window->write_index = INVALID_BUFFER;
  window->has_frame = false;

  window->x = (screen_w > width) ? static_cast<int>((screen_w - width) / 2) : 0;
  window->y =
      (screen_h > height + TITLE_H + 30)
          ? (30 + static_cast<int>((screen_h - 30 - TITLE_H - height) / 2))
          : 30;

  mark_window_dirty(*window);
  *actual_width = width;
  *actual_height = height;
  return true;
}

void unregister_window(uint64_t thread_id) {
  SpinlockGuard guard(compositor_lock);
  for (size_t i = 0; i < MAX_WINDOWS; ++i) {
    if (windows[i].alive && windows[i].thread_id == thread_id) {
      unregister_window_index(i);
      return;
    }
  }
}

bool present(uint64_t thread_id, const uint32_t *pixels, uint32_t width,
             uint32_t height, uint32_t pitch) {
  if (!pixels || !width || !height)
    return false;

  uint32_t *destination = nullptr;
  uint32_t destination_width = 0;
  int write_index = -1;
  Window *target_window = nullptr;

  {
    SpinlockGuard guard(compositor_lock);
    for (auto &w : windows) {
      if (!w.alive || w.thread_id != thread_id)
        continue;
      if (width != w.width || height != w.height ||
          pitch < width * sizeof(uint32_t))
        return true;
      for (size_t b = 0; b < FRAME_BUFFER_COUNT; ++b) {
        if (b != w.front_index && b != w.render_index && b != w.write_index) {
          write_index = static_cast<int>(b);
          break;
        }
      }
      if (write_index < 0)
        return true;
      destination = w.pixels[write_index];
      destination_width = w.width;
      w.write_index = static_cast<uint8_t>(write_index);
      target_window = &w;
      break;
    }
  }
  if (write_index < 0 || (destination == nullptr) || (target_window == nullptr))
    return false;

  const uint8_t *src_row = reinterpret_cast<const uint8_t *>(pixels);
  const uint32_t row_bytes = destination_width * sizeof(uint32_t);
  if (pitch == row_bytes) {
    memcpy(destination, pixels, static_cast<size_t>(row_bytes) * height);
  } else {
    uint8_t *dst_row = reinterpret_cast<uint8_t *>(destination);
    for (uint32_t row = 0; row < height; ++row) {
      memcpy(dst_row, src_row, row_bytes);
      dst_row += row_bytes;
      src_row += pitch;
    }
  }

  SpinlockGuard guard(compositor_lock);
  if (std::cmp_equal(target_window->write_index, write_index)) {
    target_window->write_index = INVALID_BUFFER;
    if (target_window->alive) {
      target_window->front_index = static_cast<uint8_t>(write_index);
      if (!target_window->has_frame) {
        target_window->has_frame = true;
        mark_window_dirty(*target_window);
      } else {
        mark_dirty(target_window->x, target_window->y + TITLE_H,
                   target_window->x + static_cast<int>(target_window->width),
                   target_window->y + TITLE_H +
                       static_cast<int>(target_window->height));
      }
    } else if (target_window->render_index == INVALID_BUFFER) {
      free_window_pixels(*target_window);
    }
  }
  return true;
}

bool has_window(uint64_t thread_id) {
  SpinlockGuard guard(compositor_lock);
  return find_window(thread_id) != nullptr;
}

bool title_is_open(const char *title) {
  if (!title)
    return false;
  SpinlockGuard guard(compositor_lock);
  for (const auto &w : windows) {
    if (w.alive && strcmp(w.title, title) == 0)
      return true;
  }
  return false;
}

bool has_keyboard_focus(uint64_t thread_id) {
  SpinlockGuard guard(compositor_lock);
  return window_count > 0 && focused_index_in_z < window_count &&
         windows[z_order[focused_index_in_z]].alive &&
         windows[z_order[focused_index_in_z]].thread_id == thread_id;
}

bool get_window_position(uint64_t thread_id, int *x, int *y) {
  if (!thread_id || !x || !y)
    return false;
  SpinlockGuard guard(compositor_lock);
  if (auto *w = find_window(thread_id)) {
    *x = w->x;
    *y = w->y + TITLE_H;
    return true;
  }
  return false;
}

void run() {
  std::array<RenderWindow, MAX_WINDOWS> rendered_windows{};
  for (;;) {
    size_t rendered_count = 0;
    int left = 0, top = 0, right = 0, bottom = 0;
    bool has_damage = false;
    {
      SpinlockGuard guard(compositor_lock);
      if (cursor_x < 0) {
        cursor_x = Kernel::Drivers::Mouse::get_x();
        cursor_y = Kernel::Drivers::Mouse::get_y();
        mark_dirty(0, 0, screen_w, screen_h);
      }
      mouse_update();
      has_damage = take_damage_locked(rendered_windows.data(), rendered_count,
                                      left, top, right, bottom);
    }
    if (has_damage) {
      render_region(rendered_windows.data(), rendered_count, left, top, right,
                    bottom);
      if (rendered_count)
        release_render_buffers(rendered_windows.data(), rendered_count);
    }
    Kernel::Scheduler::thread_sleep_ms(4);
  }
}

} // namespace Kernel::Graphics::WindowManager