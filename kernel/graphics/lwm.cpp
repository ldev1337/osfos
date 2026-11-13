#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include <arch/x86_64/spinlock.h>
#include <arch/x86_64/string.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/mouse.h>
#include <kernel/graphics/simplefonts.h>
#include <kernel/graphics/open_font.h>
#include <kernel/graphics/vfb.h>
#include <kernel/graphics/lwm.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>
#include <kernel/init.h>
#include <kernel/shell/shell.h>

namespace Kernel::Graphics::WindowManager {
namespace {

constexpr size_t MAX_WINDOWS = 32, MAX_DAMAGE = 4;
constexpr int TITLE_H = 26, WINDOW_RADIUS = 12, SHADOW_EXTENT = 14;
constexpr int STRIP_ROWS_MAX = 16, RETRY = -2, MAX_WAIT_TRIES = 16;
constexpr uint32_t DESKTOP = 0xFF101216, TITLE_ACTIVE = 0xFF28232F,
                   TITLE_INACTIVE = 0xFF211E29, TEXT = 0xFFF0F5F4,
                   BODY = 0xFF171B20, SHADOW = 0xFF020609;
constexpr uint32_t LIGHTS[3][2] = {{0xFFFF6B67, 0xFFE44845},
                                   {0xFFFFC45C, 0xFFE5A535},
                                   {0xFF5FD39A, 0xFF39A96D}};

struct Rect { int l, t, r, b; };
constexpr Rect EMPTY{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};

// One presented surface per window (the app's VFB is the only other copy).
// `rendering`: compositor is reading `pixels`.  `copying`: present() is
// writing `pixels`.  The two never overlap, so a single surface is tear-free.
// Invariant: z_order[0..window_count) is exactly the set of alive windows,
// bottom to top; the focused window is always the last one.
struct Window {
  uint64_t thread_id;
  char title[32];
  uint32_t *pixels;
  uint32_t width, height;
  int x, y;
  bool alive, has_frame, rendering, copying;
};

struct RenderWindow {
  Window *owner;
  const char *title;
  const uint32_t *pixels;
  uint32_t width, height;
  int x, y;
  bool focused;
};

struct Frame { // everything the compositor needs, snapshotted under the lock
  std::array<RenderWindow, MAX_WINDOWS> wins;
  std::array<Rect, MAX_DAMAGE> rects;
  size_t nwin = 0, nrect = 0;
};

Kernel::Graphics::TrueTypeFont g_font;
bool g_font_ok = false;

std::array<Window, MAX_WINDOWS> windows{};
std::array<size_t, MAX_WINDOWS> z_order{};
size_t window_count = 0;

uint32_t screen_w = 0, screen_h = 0;
int drag_x = 0, drag_y = 0, cursor_x = -1, cursor_y = -1;
bool dragging = false, old_left = false;
Spinlock compositor_lock;
uint32_t *strip_buf = nullptr; // scratch for compositing, screen_w * strip_rows
int strip_rows = 0;
std::array<Rect, MAX_DAMAGE> damage;
size_t damage_n = 0;

// ---- geometry / damage ----------------------------------------------------

bool overlaps(const Rect &a, const Rect &b) {
  return a.l < b.r && b.l < a.r && a.t < b.b && b.t < a.b;
}
Rect unite(const Rect &a, const Rect &b) {
  return {std::min(a.l, b.l), std::min(a.t, b.t), std::max(a.r, b.r),
          std::max(a.b, b.b)};
}
template <class W> Rect bounds(const W &w) { // window + titlebar + shadow
  return {w.x - SHADOW_EXTENT, w.y - SHADOW_EXTENT,
          w.x + int(w.width) + SHADOW_EXTENT,
          w.y + TITLE_H + int(w.height) + SHADOW_EXTENT};
}

// Small set of dirty rects: merge into an overlapping one, else take a free
// slot, else grow the last. Keeps e.g. cursor motion and an animating app
// from collapsing into one screen-sized bounding box.
void mark_dirty(const Rect &r) {
  if (r.l >= r.r || r.t >= r.b)
    return;
  size_t i = 0;
  while (i < damage_n && !overlaps(damage[i], r))
    ++i;
  if (i == damage_n) {
    if (damage_n < MAX_DAMAGE) {
      damage[damage_n++] = r;
      return;
    }
    i = MAX_DAMAGE - 1;
  }
  damage[i] = unite(damage[i], r);
}
template <class W> void mark_window_dirty(const W &w) { mark_dirty(bounds(w)); }

// ---- window table ---------------------------------------------------------

void copy_name(char *dst, const char *src) {
  size_t i = 0;
  for (; src && i < 31 && src[i]; ++i)
    dst[i] = src[i];
  dst[i] = '\0';
}

void free_window_pixels(Window &w) {
  if (w.pixels) {
    kfree(w.pixels);
    w.pixels = nullptr;
  }
}

uint32_t *allocate_surface(uint32_t width, uint32_t height) {
  return static_cast<uint32_t *>(kmalloc(size_t(width) * height * 4));
}

bool window_busy(const Window &w) { return w.rendering || w.copying; }

Window *find_window(uint64_t thread_id) {
  if (thread_id)
    for (auto &w : windows)
      if (w.alive && w.thread_id == thread_id)
        return &w;
  return nullptr;
}

// Moves window `idx` to the top of the z-order; false if it isn't in it.
bool raise(size_t idx) {
  const auto end = z_order.begin() + window_count;
  const auto it = std::find(z_order.begin(), end, idx);
  if (it == end)
    return false;
  std::rotate(it, it + 1, end);
  return true;
}

void drop_window(Window &w) {
  mark_window_dirty(w);
  w.alive = false;
  if (!window_busy(w))
    free_window_pixels(w); // otherwise freed by whoever finishes with it
  if (raise(size_t(&w - windows.data())))
    --window_count;
}

int register_window_locked(uint64_t thread_id, const char *title, bool launcher,
                           uint32_t width, uint32_t height) {
  if (!Kernel::Drivers::Framebuffer::valid() || !thread_id)
    return -1;

  size_t slot = MAX_WINDOWS;
  bool busy_dead = false;
  for (size_t i = 0; i < MAX_WINDOWS; ++i) {
    const Window &w = windows[i];
    if (w.alive) {
      if (w.thread_id == thread_id) {
        raise(i);
        return int(window_count - 1);
      }
    } else if (window_busy(w)) {
      busy_dead = true; // surface still in use; can't recycle yet
    } else if (slot == MAX_WINDOWS) {
      slot = i;
    }
  }

  // Never refuse an app: evict bottom-most window if at limit
  if (slot == MAX_WINDOWS) {
    if (busy_dead)
      return RETRY;
    slot = z_order[0];
    const uint64_t evict_tid = windows[slot].thread_id;
    drop_window(windows[slot]);
    Kernel::Scheduler::terminate_thread(evict_tid);
    if (window_busy(windows[slot]))
      return RETRY;
  }

  if (!width)
    width = launcher ? (screen_w > 48 ? screen_w - 48 : screen_w) : 320;
  if (!height)
    height = launcher
                 ? (screen_h > 100 ? screen_h - 100 : screen_h - TITLE_H - 2)
                 : 200;

  auto &w = windows[slot];
  free_window_pixels(w);
  uint32_t *surface = allocate_surface(width, height);
  if (!surface)
    return -1;

  const int off = int(slot % 8) * 26, margin = launcher ? 24 : 0;
  const int x = margin + off + int(width) <= int(screen_w) ? margin + off : 0;
  int y = (launcher ? 24 : 32) + off;
  if (y + int(height) + TITLE_H > int(screen_h))
    y = 30;

  w = Window{thread_id, {}, surface, width, height, x, y, true, false, false, false};
  copy_name(w.title, title);
  z_order[window_count++] = slot;
  mark_window_dirty(w);
  return int(window_count - 1);
}

// ---- rendering ------------------------------------------------------------

void draw_glyph(VirtualFramebuffer &fb, int gx, int gy, char ch, uint32_t color,
                int cl, int ct, int cr, int cb) {
  const auto c = static_cast<unsigned char>(ch);
  const auto *bits =
      Kernel::Graphics::Font::Basic8x8[(c >= 32 && c <= 127) ? c - 32
                                                             : '?' - 32];
  const int x0 = std::max(0, cl - gx), x1 = std::min(8, cr - gx);
  const int y0 = std::max(0, ct - gy), y1 = std::min(8, cb - gy);
  uint32_t *const buf = fb.buffer();
  const size_t stride = fb.width();
  for (int py = y0; py < y1; ++py) {
    uint32_t *dst = buf + (gy + py - ct) * stride + (gx - cl);
    for (int px = x0; px < x1; ++px)
      if ((bits[py] >> px) & 1)
        dst[px] = color;
  }
}

// Composites one horizontal strip into the shared scratch buffer and blits it.
void render_strip(const RenderWindow *wins, size_t count, const Rect &s) {
  const int left = s.l, top = s.t, right = s.r, bottom = s.b;
  const int sw = right - left, sh = bottom - top;
  VirtualFramebuffer fb; // non-owning view of strip_buf
  if (!fb.attach(strip_buf, left, top, sw, sh))
    return;
  fb.fill_rect(0, 0, sw, sh, DESKTOP);

  const bool ttf = g_font_ok && g_font.is_valid();
  // Clipped fill (alpha 255 = opaque) in screen coordinates.
  auto fill = [&](int fx, int fy, int fw, int fh, uint32_t c, uint8_t a = 255) {
    const int cl = std::max(fx, left), ct = std::max(fy, top);
    const int cr = std::min(fx + fw, right), cb = std::min(fy + fh, bottom);
    if (cl >= cr || ct >= cb)
      return;
    if (a == 255)
      fb.fill_rect(cl - left, ct - top, cr - cl, cb - ct, c);
    else
      fb.fill_rect_alpha(cl - left, ct - top, cr - cl, cb - ct, c, a);
  };

  for (size_t i = 0; i < count; ++i) {
    const auto &w = wins[i];
    if (!overlaps(bounds(w), s))
      continue;

    const int x = w.x, y = w.y, ww = int(w.width), hh = int(w.height);
    const int body_top = y + TITLE_H, wh = TITLE_H + hh + 2;

    fill(x - 8, y + 2, ww + 16, wh + 18, SHADOW, 12);
    fill(x - 5, y - 2, ww + 10, wh + 12, SHADOW, 22);
    fill(x - 2, y - 3, ww + 4, wh + 7, SHADOW, 30);

    // Titlebar (its rounded rect reaches 7px into the body area).
    if (top < body_top + 7 && bottom > y) {
      const uint32_t tc = w.focused ? TITLE_ACTIVE : TITLE_INACTIVE;
      fb.fill_rounded_rect(x - left, y - top, ww, TITLE_H + 7,
                           WINDOW_RADIUS - 2, tc);
      fill(x, body_top - 8, ww, 12, tc);
      fill(x, body_top - 1, ww, 1, w.focused ? 0xFF40515A : 0xFF35434A);

      if (top < body_top) {
        char title[32];
        copy_name(title, w.title);
        auto text_w = [&] {
          return ttf ? g_font.get_text_width(13.0f, title)
                     : int(strlen(title) * 8);
        };
        int mw = text_w();
        for (size_t len = strlen(title); len && mw > ww - 78; mw = text_w())
          title[--len] = '\0';
        const int tx = std::max(x + 62, x + (ww - mw) / 2), ty = y + 10;
        if (ttf) {
          fb.draw_text_truetype(tx - left, ty - top, 13.0f, g_font, title, TEXT);
        } else {
          const char *p = title;
          const int cr = std::min(x + ww - 12, right);
          for (int cx = tx; *p && cx < cr; ++p, cx += 8)
            draw_glyph(fb, cx, ty, *p, TEXT, left, top, cr, bottom);
        }
        for (int k = 0; k < 3; ++k) {
          const int cx = x + 18 + 16 * k - left, cy = y + TITLE_H / 2 - top;
          fb.fill_circle(cx, cy, 5, LIGHTS[k][0]);
          fb.fill_circle(cx, cy, 2, LIGHTS[k][1]);
        }
      }
    }

    // Body. Corners only round the bottom `rad` rows; everything above is a
    // plain rectangle, so it is copied with a single blit.
    const int rad = std::min(WINDOW_RADIUS - 2, std::min(ww, hh) / 2);
    const int round_top = body_top + hh - rad;
    if (!w.pixels || (rad > 0 && bottom > round_top)) { // bg for AA'd corners
      fb.fill_rounded_rect(x - left, body_top - top, ww, hh, rad, BODY);
      if (!w.pixels)
        fill(x, body_top, ww, rad, BODY);
    }
    if (w.pixels) {
      const int cl = std::max(x, left), cr = std::min(x + ww, right);
      const int ct = std::max(body_top, top), cb = std::min(body_top + hh, bottom);
      auto blit_rows = [&](int x0, int x1, int y0, int y1) {
        if (x0 < x1 && y0 < y1)
          fb.blit(w.pixels + size_t(y0 - body_top) * ww + (x0 - x), x0 - left,
                  y0 - top, x1 - x0, y1 - y0, ww * sizeof(uint32_t));
      };
      blit_rows(cl, cr, ct, std::min(cb, round_top));
      for (int py = std::max(ct, round_top); py < cb; ++py) {
        const int dy = py - round_top;
        int inset = 0;
        while (inset < rad && (rad - inset) * (rad - inset) + dy * dy > rad * rad)
          ++inset;
        blit_rows(std::max(cl, x + inset), std::min(cr, x + ww - inset), py,
                  py + 1);
      }
    }
  }

  fill(cursor_x - 1, cursor_y - 7, 3, 15, TEXT);
  fill(cursor_x - 7, cursor_y - 1, 15, 3, TEXT);
  Kernel::Drivers::Framebuffer::blit(strip_buf, left, top, sw, sh, fb.pitch());
}

void render_region(const Frame &f, const Rect &r) {
  for (int y = r.t; y < r.b; y += strip_rows)
    render_strip(f.wins.data(), f.nwin,
                 {r.l, y, r.r, std::min(y + strip_rows, r.b)});
}

// ---- input / frame hand-off (compositor lock held) ------------------------

void mouse_update() {
  const bool left = Kernel::Drivers::Mouse::raw_left_pressed();
  const int mx = Kernel::Drivers::Mouse::get_x();
  const int my = Kernel::Drivers::Mouse::get_y();
  const int px = cursor_x, py = cursor_y;
  cursor_x = mx;
  cursor_y = my;

  if (left && !old_left) {
    for (size_t n = window_count; n--;) {
      auto &w = windows[z_order[n]];
      if (mx < w.x - 1 || mx >= w.x + int(w.width) + 1 || my < w.y - 1 ||
          my >= w.y + TITLE_H + int(w.height) + 1)
        continue;
      if (my >= w.y + 5 && my < w.y + TITLE_H - 5 && mx >= w.x + 7 &&
          mx < w.x + 29) { // close button
        const uint64_t tid = w.thread_id;
        drop_window(w);
        Kernel::Scheduler::terminate_thread(tid);
      } else {
        if (n != window_count - 1) { // focus change: old titlebar + new window
          const auto &old = windows[z_order[window_count - 1]];
          mark_dirty({old.x, old.y, old.x + int(old.width), old.y + TITLE_H});
          raise(z_order[n]);
          mark_window_dirty(w);
        }
        if (my < w.y + TITLE_H) {
          dragging = true;
          drag_x = mx - w.x;
          drag_y = my - w.y;
        }
      }
      break;
    }
  }

  if (left && dragging && window_count) {
    auto &w = windows[z_order[window_count - 1]];
    const int nx = std::clamp(mx - drag_x, -int(w.width) + 32, int(screen_w) - 32);
    const int ny = std::clamp(my - drag_y, 0, int(screen_h) - TITLE_H);
    if (nx != w.x || ny != w.y) {
      mark_window_dirty(w);
      w.x = nx;
      w.y = ny;
      mark_window_dirty(w);
    }
  }

  if (!left)
    dragging = false;
  old_left = left;
  if (mx != px || my != py) {
    mark_dirty({px - 7, py - 7, px + 8, py + 8});
    mark_dirty({mx - 7, my - 7, mx + 8, my + 8});
  }
}

// Snapshots dirty rects + the windows touching them and pins their surfaces.
// If a window in the damage is mid-present() the damage stays queued for the
// next tick (copies take <1 ms).
bool take_damage_locked(Frame &f) {
  Rect all = EMPTY;
  f.nrect = 0;
  for (size_t i = 0; i < damage_n; ++i) {
    const Rect r{std::clamp(damage[i].l, 0, int(screen_w)),
                 std::clamp(damage[i].t, 0, int(screen_h)),
                 std::clamp(damage[i].r, 0, int(screen_w)),
                 std::clamp(damage[i].b, 0, int(screen_h))};
    if (r.l < r.r && r.t < r.b) {
      f.rects[f.nrect++] = r;
      all = unite(all, r);
    }
  }
  if (!f.nrect) {
    damage_n = 0;
    return false;
  }

  f.nwin = 0;
  for (size_t z = 0; z < window_count; ++z) {
    auto &w = windows[z_order[z]];
    if (!overlaps(bounds(w), all))
      continue;
    if (w.copying)
      return false;
    f.wins[f.nwin++] = {&w, w.title, w.has_frame ? w.pixels : nullptr, w.width,
                        w.height, w.x, w.y, z == window_count - 1};
  }
  for (size_t i = 0; i < f.nwin; ++i)
    f.wins[i].owner->rendering = true;
  damage_n = 0;
  return true;
}

void release_render_buffers(const Frame &f) {
  if (!f.nwin)
    return;
  SpinlockGuard guard(compositor_lock);
  for (size_t i = 0; i < f.nwin; ++i) {
    auto &w = *f.wins[i].owner;
    w.rendering = false;
    if (!w.alive && !w.copying)
      free_window_pixels(w);
  }
}

} // namespace

bool __init initialize() {
  if (!Kernel::Drivers::Framebuffer::valid())
    return false;
  screen_w = Kernel::Drivers::Framebuffer::width();
  screen_h = Kernel::Drivers::Framebuffer::height();
  if (screen_w < 240 || screen_h < 160)
    return false;

  if (!g_font_ok)
    g_font_ok = Kernel::Graphics::OpenFont::load_embedded_named(g_font, "GeistMono.ttf") ||
                Kernel::Graphics::OpenFont::load_preferred(g_font, "GeistMono.ttf", "Geist.ttf");

  if (strip_buf)
    kfree(strip_buf);
  strip_buf = nullptr;
  strip_rows = 0;
  for (int rows = STRIP_ROWS_MAX; rows >= 1 && !strip_buf; rows /= 2)
    if ((strip_buf = static_cast<uint32_t *>(kmalloc(size_t(screen_w) * rows * 4))))
      strip_rows = rows;
  return strip_buf != nullptr;
}

int register_window(uint64_t thread_id, const char *title, bool launcher,
                    uint32_t width, uint32_t height) {
  for (int attempt = 0; attempt < MAX_WAIT_TRIES; ++attempt) {
    {
      SpinlockGuard guard(compositor_lock);
      const int r =
          register_window_locked(thread_id, title, launcher, width, height);
      if (r != RETRY)
        return r;
    }
    Kernel::Scheduler::thread_sleep_ms(1);
  }
  return -1;
}

bool request_window_size(uint64_t thread_id, uint32_t rw, uint32_t rh,
                         uint32_t *actual_width, uint32_t *actual_height) {
  if (!thread_id || !rw || !rh || !actual_width || !actual_height)
    return false;

  for (int attempt = 0; attempt < MAX_WAIT_TRIES; ++attempt) {
    {
      SpinlockGuard guard(compositor_lock);
      Window *w = find_window(thread_id);
      if (!w) {
        const int r = register_window_locked(thread_id, "App", false, rw, rh);
        if (r < 0 && r != RETRY)
          return false;
        if (r >= 0 && !(w = find_window(thread_id)))
          return false;
      } else if (w->width != rw || w->height != rh) {
        if (window_busy(*w)) {
          w = nullptr; // compositor is reading the surface
        } else if (uint32_t *surface = allocate_surface(rw, rh)) {
          mark_window_dirty(*w);
          free_window_pixels(*w);
          w->pixels = surface;
          w->width = rw;
          w->height = rh;
          w->has_frame = false;
          w->x = screen_w > rw ? int((screen_w - rw) / 2) : 0;
          w->y = screen_h > rh + TITLE_H + 30
                     ? 30 + int((screen_h - 30 - TITLE_H - rh) / 2)
                     : 30;
          mark_window_dirty(*w);
        } // else: allocation failed, report the current size
      }
      if (w) {
        *actual_width = w->width;
        *actual_height = w->height;
        return true;
      }
    }
    Kernel::Scheduler::thread_sleep_ms(1);
  }

  SpinlockGuard guard(compositor_lock);
  if (auto *w = find_window(thread_id)) {
    *actual_width = w->width;
    *actual_height = w->height;
    return true;
  }
  return false;
}

void unregister_window(uint64_t thread_id) {
  SpinlockGuard guard(compositor_lock);
  if (auto *w = find_window(thread_id))
    drop_window(*w);
}

bool present(uint64_t thread_id, const uint32_t *pixels, uint32_t width,
             uint32_t height, uint32_t pitch) {
  if (!pixels || !width || !height)
    return false;

  Window *target = nullptr;
  uint32_t *dst = nullptr;
  for (int attempt = 0; attempt < MAX_WAIT_TRIES && !target; ++attempt) {
    {
      SpinlockGuard guard(compositor_lock);
      auto *w = find_window(thread_id);
      if (!w)
        return false;
      if (width != w->width || height != w->height ||
          pitch < width * sizeof(uint32_t))
        return true;
      if (!window_busy(*w)) {
        w->copying = true;
        target = w;
        dst = w->pixels;
      }
    }
    if (!target)
      Kernel::Scheduler::thread_sleep_ms(1); // compositor is reading
  }
  if (!target)
    return true; // compositor starved us; drop this frame

  const size_t row_bytes = size_t(width) * sizeof(uint32_t);
  auto *src = reinterpret_cast<const uint8_t *>(pixels);
  if (pitch == row_bytes) {
    memcpy(dst, src, row_bytes * height);
  } else {
    for (auto *d = reinterpret_cast<uint8_t *>(dst); height--;
         d += row_bytes, src += pitch)
      memcpy(d, src, row_bytes);
  }

  SpinlockGuard guard(compositor_lock);
  target->copying = false;
  if (!target->alive) {
    if (!target->rendering)
      free_window_pixels(*target);
  } else if (!target->has_frame) {
    target->has_frame = true;
    mark_window_dirty(*target);
  } else { // client area only
    mark_dirty({target->x, target->y + TITLE_H, target->x + int(target->width),
                target->y + TITLE_H + int(target->height)});
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
  for (const auto &w : windows)
    if (w.alive && strcmp(w.title, title) == 0)
      return true;
  return false;
}

uint64_t keyboard_focus_thread_id() {
  SpinlockGuard guard(compositor_lock);
  return window_count ? windows[z_order[window_count - 1]].thread_id : 0;
}

bool has_keyboard_focus(uint64_t thread_id) {
  return thread_id && keyboard_focus_thread_id() == thread_id;
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
  Frame f;
  for (;;) {
    bool damaged;
    {
      SpinlockGuard guard(compositor_lock);
      if (cursor_x < 0) {
        cursor_x = Kernel::Drivers::Mouse::get_x();
        cursor_y = Kernel::Drivers::Mouse::get_y();
        mark_dirty({0, 0, int(screen_w), int(screen_h)});
      }
      mouse_update();
      damaged = take_damage_locked(f);
    }
    if (damaged) {
      for (size_t i = 0; i < f.nrect; ++i)
        render_region(f, f.rects[i]);
      release_render_buffers(f);
    }
    Kernel::Scheduler::thread_sleep_ms(4);
  }
}

} // namespace Kernel::Graphics::WindowManager