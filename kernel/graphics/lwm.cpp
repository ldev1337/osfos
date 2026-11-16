// lwm.cpp - window manager / compositor with NO per-window pixel memory.
//
// Windows own no surface. Applications draw straight into the hardware
// framebuffer (through vfb.cpp), clipped to the part of their client area that
// no window above them covers. The compositor paints the desktop, shadows,
// title bars, empty window bodies and the software cursor.
//
// Pixel writes are deliberately unsynchronized: applications and the
// compositor may write the framebuffer concurrently. Tearing and lost pixel
// updates are accepted. compositor_lock protects only window-manager metadata
// and is never held while application primitives or compositor painting run.
//
// The software cursor has no save-under. Its old rectangle is damaged when it
// moves so the compositor can repaint that area; applications may need to
// redraw exposed client pixels there.
#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>

#include <arch/x86_64/spinlock.h>
#include <arch/x86_64/string.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/mouse.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/lwm_draw.h>
#include <kernel/graphics/open_font.h>
#include <kernel/graphics/simplefonts.h>
#include <kernel/graphics/vfb.h>
#include <kernel/init.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>
#include <kernel/shell/shell.h>

namespace Kernel::Graphics::WindowManager {
namespace {

constexpr size_t MAX_WINDOWS = 32, MAX_DAMAGE = 8;
constexpr int TITLE_H = 26, WINDOW_RADIUS = 12, CORNER = WINDOW_RADIUS - 2;
constexpr uint32_t DESKTOP = 0xFF101216, TITLE_ACTIVE = 0xFF28232F,
                   TITLE_INACTIVE = 0xFF211E29, TEXT = 0xFFF0F5F4,
                   BLACK = 0x00000000, BODY = 0xFF171B20, SHADOW = 0xFF020609;
constexpr uint32_t LIGHTS[3][2] = {{0xFFFF6B67, 0xFFE44845},
                                   {0xFFFFC45C, 0xFFE5A535},
                                   {0xFF5FD39A, 0xFF39A96D}};
constexpr uint8_t SHADOW_LAYER_ALPHA[3] = {12, 22, 30};
constexpr int CURSOR_R = 7, CURSOR_SPAN = 2 * CURSOR_R + 1;

// ---- geometry -------------------------------------------------------------

struct Rect {
  int l, t, r, b;
};

inline bool is_empty(const Rect &r) { return r.l >= r.r || r.t >= r.b; }
inline bool overlaps(const Rect &a, const Rect &b) {
  return a.l < b.r && b.l < a.r && a.t < b.b && b.t < a.b;
}
inline Rect unite(const Rect &a, const Rect &b) {
  return {std::min(a.l, b.l), std::min(a.t, b.t), std::max(a.r, b.r),
          std::max(a.b, b.b)};
}
inline Rect inter(const Rect &a, const Rect &b) {
  return {std::max(a.l, b.l), std::max(a.t, b.t), std::min(a.r, b.r),
          std::min(a.b, b.b)};
}

// Fixed-capacity rectangle set. add() drops overflow, add_or_grow() merges.
template <size_t N> struct RectList {
  std::array<Rect, N> r;
  size_t n = 0;

  void add(const Rect &x) {
    if (!is_empty(x) && n < N)
      r[n++] = x;
  }
  void add_or_grow(const Rect &x) {
    if (is_empty(x))
      return;
    if (n < N)
      r[n++] = x;
    else
      r[N - 1] = unite(r[N - 1], x);
  }
  void subtract(const Rect &h) {
    RectList out;
    for (size_t i = 0; i < n; ++i) {
      const Rect &a = r[i];
      if (!overlaps(a, h)) {
        out.add(a);
        continue;
      }
      const int mt = std::max(a.t, h.t), mb = std::min(a.b, h.b);
      if (a.t < h.t)
        out.add({a.l, a.t, a.r, h.t});
      if (a.b > h.b)
        out.add({a.l, h.b, a.r, a.b});
      if (a.l < h.l)
        out.add({a.l, mt, h.l, mb});
      if (a.r > h.r)
        out.add({h.r, mt, a.r, mb});
    }
    *this = out;
  }
};

// A window is title bar + client area; its drop shadow reaches 8px to the
// sides, 3px above and 22px below (union of the three shadow layers).
inline Rect bounds_at(int x, int y, uint32_t w, uint32_t h) {
  return {x - 8, y - 3, x + int(w) + 8, y + TITLE_H + int(h) + 22};
}
inline Rect client_at(int x, int y, uint32_t w, uint32_t h) {
  return {x, y + TITLE_H, x + int(w), y + TITLE_H + int(h)};
}
template <class W> Rect bounds(const W &w) {
  return bounds_at(w.x, w.y, w.width, w.height);
}
template <class W> Rect client(const W &w) {
  return client_at(w.x, w.y, w.width, w.height);
}

// ---- state ----------------------------------------------------------------

struct Window {
  uint64_t thread_id;
  char title[32];
  uint32_t width, height;
  int x, y;   // where the window should be
  int sx, sy; // where its pixels currently are (valid if `placed`)
  bool alive;
  bool placed;
  bool exposed;
};

struct RenderWindow {
  char title[32];
  uint32_t width, height;
  int x, y;
  int tx;
  bool focused;
};

struct Frame {
  std::array<RenderWindow, MAX_WINDOWS> wins;
  size_t nwin = 0;
  std::array<Rect, (MAX_DAMAGE + 4) * 4> pieces;
  size_t npiece = 0;
  bool move = false;
  Rect move_from{}, move_to{};
  int cx = 0, cy = 0;
};

std::array<Window, MAX_WINDOWS> windows{};
std::array<size_t, MAX_WINDOWS> z_order{}; // bottom..top, last = focused
size_t window_count = 0;

uint32_t screen_w = 0, screen_h = 0;
uint32_t *fb = nullptr;
size_t fb_stride = 0;
int drag_x = 0, drag_y = 0, cursor_x = -1, cursor_y = -1;
bool dragging = false, old_left = false;
Spinlock compositor_lock;
std::array<Rect, MAX_DAMAGE> damage;
size_t damage_n = 0;

Frame g_frame;
uint8_t g_shadow_alpha[8];

// Cursor state is compositor-owned metadata. There is deliberately no
// framebuffer save-under and no attempt to exclude concurrent application
// writes.
bool cursor_painted = false;
int cursor_painted_x = 0, cursor_painted_y = 0;

inline uint32_t *fb_row(int y) { return fb + size_t(y) * fb_stride; }

// ---- damage ---------------------------------------------------------------

void mark_dirty(const Rect &r) {
  if (is_empty(r))
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

void mark_window_dirty(const Window &w) {
  mark_dirty(bounds(w));
  if (w.placed && (w.sx != w.x || w.sy != w.y))
    mark_dirty(bounds_at(w.sx, w.sy, w.width, w.height));
}

// ---- window table ---------------------------------------------------------

void copy_name(char *dst, const char *src) {
  size_t i = 0;
  for (; src && i < 31 && src[i]; ++i)
    dst[i] = src[i];
  dst[i] = '\0';
}

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

void focus_window(size_t idx) {
  const auto end = z_order.begin() + window_count;
  const auto it = std::find(z_order.begin(), end, idx);
  if (it == end || it + 1 == end)
    return;
  const Window &w = windows[idx];
  const Window &top = windows[*(end - 1)];
  mark_dirty({top.x, top.y, top.x + int(top.width), top.y + TITLE_H});
  mark_dirty({w.x, w.y, w.x + int(w.width), w.y + TITLE_H});
  const Rect wb = bounds(w);
  for (auto j = it + 1; j != end; ++j) {
    const Rect ab = bounds(windows[*j]);
    if (overlaps(ab, wb))
      mark_dirty(inter(ab, wb));
  }
  std::rotate(it, it + 1, end);
}

void drop_window(Window &w) {
  const size_t idx = size_t(&w - windows.data());
  mark_window_dirty(w);
  w.alive = false;
  if (window_count && z_order[window_count - 1] == idx)
    dragging = false;
  if (raise(idx))
    --window_count;
}

int register_window_locked(uint64_t thread_id, const char *title, bool launcher,
                           uint32_t width, uint32_t height) {
  if (!Kernel::Drivers::Framebuffer::valid() || !thread_id)
    return -1;

  size_t slot = MAX_WINDOWS;
  for (size_t i = 0; i < MAX_WINDOWS; ++i) {
    const Window &w = windows[i];
    if (w.alive) {
      if (w.thread_id == thread_id) {
        focus_window(i);
        return int(window_count - 1);
      }
    } else if (slot == MAX_WINDOWS) {
      slot = i;
    }
  }

  if (slot == MAX_WINDOWS) {
    slot = z_order[0];
    const uint64_t evict_tid = windows[slot].thread_id;
    drop_window(windows[slot]);
    Kernel::Scheduler::terminate_thread(evict_tid);
  }

  if (!width)
    width = launcher ? (screen_w > 48 ? screen_w - 48 : screen_w) : 320;
  if (!height)
    height = launcher
                 ? (screen_h > 100 ? screen_h - 100 : screen_h - TITLE_H - 2)
                 : 200;

  const int off = int(slot % 8) * 26, margin = launcher ? 24 : 0;
  const int x = margin + off + int(width) <= int(screen_w) ? margin + off : 0;
  int y = (launcher ? 24 : 32) + off;
  if (y + int(height) + TITLE_H > int(screen_h))
    y = 30;

  auto &w = windows[slot];
  w = Window{thread_id, {}, width, height, x, y, x, y, true, false, true};
  copy_name(w.title, title);
  z_order[window_count++] = slot;
  dragging = false;
  mark_window_dirty(w);
  return int(window_count - 1);
}

// ---- software cursor ------------------------------------------------------
// No save-under: the cursor is simply overwritten at its current location.
// When it moves, the previous rectangle is repainted through normal damage.
inline int cursor_shape(int dx, int dy) {
  if ((dx == 0 && dy >= -6 && dy < 6) || (dy == 0 && dx >= -6 && dx < 6))
    return 2;
  if ((dx >= -1 && dx <= 1 && dy >= -7 && dy < 7) ||
      (dy >= -1 && dy <= 1 && dx >= -7 && dx < 7))
    return 1;
  return 0;
}

inline Rect cursor_rect(int x, int y) {
  return {x - CURSOR_R, y - CURSOR_R, x + CURSOR_R + 1, y + CURSOR_R + 1};
}

void cursor_show(int cx, int cy) {
  for (int dy = -CURSOR_R; dy <= CURSOR_R; ++dy) {
    const int y = cy + dy;
    if (unsigned(y) >= screen_h)
      continue;
    uint32_t *row = fb_row(y);
    for (int dx = -CURSOR_R; dx <= CURSOR_R; ++dx) {
      const int x = cx + dx, shape = cursor_shape(dx, dy);
      if (!shape || unsigned(x) >= screen_w)
        continue;
      row[x] = shape == 1 ? BLACK : TEXT;
    }
  }
  cursor_painted_x = cx;
  cursor_painted_y = cy;
  cursor_painted = true;
}

// ---- painting (compositor only; writes directly to the framebuffer) ------

void paint_shadow(const Canvas &c, const RenderWindow &w) {
  const int x = w.x, y = w.y, ww = int(w.width);
  const int full_h = TITLE_H + int(w.height), wh = full_h + 2;
  const int top[3] = {y + 2, y - 2, y - 3};
  const int bot[3] = {y + 2 + wh + 18, y - 2 + wh + 12, y - 3 + wh + 7};
  const int edge[6] = {x - 8, x - 5, x - 2, x + ww + 2, x + ww + 5, x + ww + 8};
  constexpr unsigned zone[5] = {1, 3, 7, 3, 1};

  for (int py = std::max(c.t, y - 3), pe = std::min(c.b, bot[0]); py < pe;
       ++py) {
    unsigned rm = 0;
    for (int k = 0; k < 3; ++k)
      rm |= (py >= top[k] && py < bot[k]) ? 1u << k : 0;
    if (!rm)
      continue;
    int sl = 0, sr = 0;
    if (py >= y + WINDOW_RADIUS && py < y + full_h - WINDOW_RADIUS) {
      sl = x;
      sr = x + ww;
    } else if (py >= y && py < y + full_h && ww > 2 * WINDOW_RADIUS) {
      sl = x + WINDOW_RADIUS;
      sr = x + ww - WINDOW_RADIUS;
    }
    uint32_t *row = c.origin + ptrdiff_t(py) * c.stride;
    for (int z = 0; z < 5; ++z) {
      const unsigned m = zone[z] & rm;
      const int a = std::max(edge[z], c.l), e = std::min(edge[z + 1], c.r);
      if (!m || a >= e)
        continue;
      const uint32_t alpha = g_shadow_alpha[m];
      auto shade = [&](int from, int to) {
        for (int px = from; px < to; ++px)
          row[px] = Raster::blend(SHADOW, row[px], alpha);
      };
      if (sl >= sr) {
        shade(a, e);
      } else {
        shade(a, std::min(e, sl));
        shade(std::max(a, sr), e);
      }
    }
  }
}

void paint_title(const Canvas &c, const RenderWindow &w, TrueTypeFont *font) {
  const int x = w.x, y = w.y, ww = int(w.width), body_top = y + TITLE_H;
  if (c.t >= body_top || c.b <= y)
    return;
  Canvas t = c;
  t.b = std::min(c.b, body_top);
  const bool f = w.focused;
  Raster::fill_rounded_rect(t, x, y, ww, TITLE_H + CORNER, CORNER,
                            f ? TITLE_ACTIVE : TITLE_INACTIVE);
  Raster::fill_rect(t, x, body_top - 1, ww, 1,
                    f ? 0xFF40515A : 0xFF35434A);
  const int ty = y + 10;
  if (font) {
    Raster::text(t, *font, w.tx, ty, 13.0f, w.title, TEXT);
  } else {
    const int limit = x + ww - 12;
    const char *s = w.title;
    for (int cx = w.tx; *s && cx < limit; ++s, cx += 8)
      Raster::glyph8(t, cx, ty, *s, TEXT);
  }
  for (int k = 0; k < 3; ++k) {
    const int cx = x + 18 + 16 * k, cy = y + TITLE_H / 2;
    Raster::fill_circle(t, cx, cy, 5, LIGHTS[k][0]);
    Raster::fill_circle(t, cx, cy, 2, LIGHTS[k][1]);
  }
}

void paint_body(const Canvas &c, const RenderWindow &w) {
  const int ww = int(w.width), hh = int(w.height), body_top = w.y + TITLE_H;
  const int rad = std::min(CORNER, std::min(ww, hh) / 2);
  Canvas b = c;
  b.t = std::max(c.t, body_top);
  if (b.t < b.b)
    Raster::fill_rounded_rect(b, w.x, body_top - rad, ww, hh + rad, rad, BODY);
}

void paint_piece(const Frame &f, const Rect &p, TrueTypeFont *font) {
  const Canvas c{fb, ptrdiff_t(fb_stride), p.l, p.t, p.r, p.b};
  Raster::fill_rect(c, p.l, p.t, p.r - p.l, p.b - p.t, DESKTOP);
  for (size_t i = 0; i < f.nwin; ++i) {
    const RenderWindow &rw = f.wins[i];
    if (!overlaps(bounds(rw), p))
      continue;
    paint_shadow(c, rw);
    paint_title(c, rw, font);
    paint_body(c, rw);
  }
}

// Overlap-safe copy of a rectangle on the framebuffer.
void move_pixels(const Rect &from, const Rect &to) {
  const int w = to.r - to.l, h = to.b - to.t;
  const int dx = to.l - from.l, dy = to.t - from.t;
  for (int i = 0; i < h; ++i) {
    const int r = dy > 0 ? h - 1 - i : i;
    uint32_t *s = fb_row(from.t + r) + from.l;
    uint32_t *d = fb_row(to.t + r) + to.l;
    if (dy != 0)
      memcpy(d, s, size_t(w) * 4);
    else if (dx < 0)
      for (int k = 0; k < w; ++k)
        d[k] = s[k];
    else
      for (int k = w; k--;)
        d[k] = s[k];
  }
}

void place_title(RenderWindow &w, TrueTypeFont *font) {
  const int ww = int(w.width);
  auto text_w = [&] {
    return font ? font->get_text_width(13.0f, w.title)
                : int(strlen(w.title) * 8);
  };
  int mw = text_w();
  for (size_t len = strlen(w.title); len && mw > ww - 78; mw = text_w())
    w.title[--len] = '\0';
  w.tx = std::max(w.x + 62, w.x + (ww - mw) / 2);
}

void render(Frame &f) {
  TrueTypeFont *font = Raster::default_font();
  for (size_t i = 0; i < f.nwin; ++i)
    place_title(f.wins[i], font);
  if (f.move)
    move_pixels(f.move_from, f.move_to);
  for (size_t i = 0; i < f.npiece; ++i)
    paint_piece(f, f.pieces[i], font);
  // Reassert the cursor every compositor tick. Applications can overwrite it
  // at any time; the next tick writes the pointer pixels again.
  cursor_show(f.cx, f.cy);
}

// ---- input (compositor lock held) -----------------------------------------

void mouse_update() {
  const bool left = Kernel::Drivers::Mouse::raw_left_pressed();
  const int mx = cursor_x = Kernel::Drivers::Mouse::get_x();
  const int my = cursor_y = Kernel::Drivers::Mouse::get_y();

  if (left && !old_left) {
    for (size_t n = window_count; n--;) {
      auto &w = windows[z_order[n]];
      if (mx < w.x - 1 || mx >= w.x + int(w.width) + 1 || my < w.y - 1 ||
          my >= w.y + TITLE_H + int(w.height) + 1)
        continue;
      if (my >= w.y + 5 && my < w.y + TITLE_H - 5 && mx >= w.x + 7 &&
          mx < w.x + 29) {
        const uint64_t tid = w.thread_id;
        drop_window(w);
        Kernel::Scheduler::terminate_thread(tid);
      } else {
        focus_window(z_order[n]);
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
    w.x = std::clamp(mx - drag_x, -int(w.width) + 32, int(screen_w) - 32);
    w.y = std::clamp(my - drag_y, 0, int(screen_h) - TITLE_H);
  }

  if (!left)
    dragging = false;
  old_left = left;
}

// Plans compositor work while compositor_lock protects metadata. The lock is
// released before render() touches any framebuffer pixel.
bool plan_locked(Frame &f) {
  const Rect screen{0, 0, int(screen_w), int(screen_h)};
  RectList<MAX_DAMAGE + 4> dirty;
  for (size_t i = 0; i < damage_n; ++i)
    dirty.add_or_grow(damage[i]);

  const bool cursor_moved =
      cursor_painted &&
      (cursor_x != cursor_painted_x || cursor_y != cursor_painted_y);
  if (cursor_moved)
    dirty.add_or_grow(cursor_rect(cursor_painted_x, cursor_painted_y));

  // The top window moves by copying its client pixels on the framebuffer.
  // Do not use this optimization if the cursor is in the copied region: the
  // cursor is part of the raw framebuffer now and must not be copied as content.
  bool keep = false;
  Rect keep_from{}, keep_to{};
  for (size_t z = window_count; z--;) {
    const Window &w = windows[z_order[z]];
    if (!w.placed) {
      dirty.add_or_grow(bounds(w));
      continue;
    }
    if (w.x == w.sx && w.y == w.sy)
      continue;
    dirty.add_or_grow(bounds_at(w.sx, w.sy, w.width, w.height));
    dirty.add_or_grow(bounds(w));
    if (z != window_count - 1)
      continue;

    const int dx = w.x - w.sx, dy = w.y - w.sy;
    const Rect src = inter(client_at(w.sx, w.sy, w.width, w.height), screen);
    bool stale = false;
    for (size_t i = 0; i < damage_n; ++i)
      stale |= overlaps(damage[i], src);
    const Rect to =
        inter({src.l + dx, src.t + dy, src.r + dx, src.b + dy}, screen);
    if (cursor_painted) {
      const Rect cr = cursor_rect(cursor_painted_x, cursor_painted_y);
      stale |= overlaps(cr, src) || overlaps(cr, to);
    }
    if (!stale && !is_empty(to)) {
      keep = true;
      keep_to = to;
      keep_from = {to.l - dx, to.t - dy, to.r - dx, to.b - dy};
    }
  }

  RectList<(MAX_DAMAGE + 4) * 4> pieces;
  for (size_t i = 0; i < dirty.n; ++i)
    pieces.add(inter(dirty.r[i], screen));
  if (keep)
    pieces.subtract(keep_to);

  Rect all{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
  f.npiece = 0;
  for (size_t i = 0; i < pieces.n; ++i) {
    f.pieces[f.npiece++] = pieces.r[i];
    all = unite(all, pieces.r[i]);
  }

  f.move = keep;
  f.move_from = keep_from;
  f.move_to = keep_to;
  f.cx = cursor_x;
  f.cy = cursor_y;

  f.nwin = 0;
  if (f.npiece) {
    for (size_t z = 0; z < window_count; ++z) {
      auto &w = windows[z_order[z]];
      if (!overlaps(bounds(w), all))
        continue;
      RenderWindow &rw = f.wins[f.nwin++];
      copy_name(rw.title, w.title);
      rw.width = w.width;
      rw.height = w.height;
      rw.x = w.x;
      rw.y = w.y;
      rw.focused = z == window_count - 1;
      w.placed = true;
      w.sx = w.x;
      w.sy = w.y;
      const Rect cl = client(w);
      for (size_t i = 0; i < f.npiece; ++i)
        if (overlaps(cl, f.pieces[i])) {
          w.exposed = true;
          break;
        }
    }
  }
  damage_n = 0;
  return true; // render every tick to reassert the unsaved cursor overlay
}

void tick() {
  {
    SpinlockGuard guard(compositor_lock);
    mouse_update();
    plan_locked(g_frame);
  }
  render(g_frame);
}

} // namespace

// ---- public API -----------------------------------------------------------

bool __init initialize() {
  if (!Kernel::Drivers::Framebuffer::valid())
    return false;
  screen_w = Kernel::Drivers::Framebuffer::width();
  screen_h = Kernel::Drivers::Framebuffer::height();
  const auto info = Kernel::Drivers::Framebuffer::get_info();
  fb = reinterpret_cast<uint32_t *>(uintptr_t(info.address));
  fb_stride = info.pitch / 4;
  if (screen_w < 240 || screen_h < 160 || !fb || !fb_stride ||
      fb_stride < screen_w)
    return false;

  Raster::default_font();
  for (unsigned m = 0; m < 8; ++m) {
    unsigned inv = 255;
    for (int k = 0; k < 3; ++k)
      if (m & (1u << k))
        inv = inv * (255 - SHADOW_LAYER_ALPHA[k]) / 255;
    g_shadow_alpha[m] = uint8_t(255 - inv);
  }
  return true;
}

int register_window(uint64_t thread_id, const char *title, bool launcher,
                    uint32_t width, uint32_t height) {
  SpinlockGuard guard(compositor_lock);
  return register_window_locked(thread_id, title, launcher, width, height);
}

bool request_window_size(uint64_t thread_id, uint32_t rw, uint32_t rh,
                         uint32_t *actual_width, uint32_t *actual_height) {
  SpinlockGuard guard(compositor_lock);
  Window *w = find_window(thread_id);
  if (!w) {
    if (register_window_locked(thread_id, "App", false, rw, rh) < 0 ||
        !(w = find_window(thread_id)))
      return false;
  } else if (w->width != rw || w->height != rh) {
    mark_window_dirty(*w);
    w->width = rw;
    w->height = rh;
    w->placed = false;
    w->exposed = true;
    w->x = screen_w > rw ? int((screen_w - rw) / 2) : 0;
    w->y = screen_h > rh + TITLE_H + 30
               ? 30 + int((screen_h - 30 - TITLE_H - rh) / 2)
               : 30;
    mark_window_dirty(*w);
  }
  *actual_width = w->width;
  *actual_height = w->height;
  return true;
}

void unregister_window(uint64_t thread_id) {
  SpinlockGuard guard(compositor_lock);
  if (auto *w = find_window(thread_id))
    drop_window(*w);
}

// Lock-free with respect to framebuffer drawing. This short critical section
// only snapshots window geometry and z-order to build the caller's clip list.
DrawStatus begin_draw(uint64_t thread_id, bool windowed, int32_t fx, int32_t fy,
                      int32_t bx, int32_t by, int32_t bw, int32_t bh,
                      Surface &s) {
  (void)bx;
  (void)by;
  (void)bw;
  (void)bh;
  SpinlockGuard guard(compositor_lock);
  Window *w = find_window(thread_id);
  if (!w && windowed)
    return DrawStatus::NoWindow;

  RectList<kMaxClip> clip;
  if (w) {
    s.ox = w->x;
    s.oy = w->y + TITLE_H;
    s.win_w = w->width;
    s.win_h = w->height;
    const Rect cl = client(*w);
    clip.add(inter(cl, {0, 0, int(screen_w), int(screen_h)}));
    size_t z = 0;
    const size_t idx = size_t(w - windows.data());
    while (z < window_count && z_order[z] != idx)
      ++z;
    if (z < window_count) {
      for (++z; z < window_count && clip.n; ++z) {
        const Rect ab = bounds(windows[z_order[z]]);
        if (overlaps(ab, cl))
          clip.subtract(ab);
      }
    }
  } else {
    s.ox = fx;
    s.oy = fy;
    s.win_w = s.win_h = 0;
    clip.add({0, 0, int(screen_w), int(screen_h)});
  }

  s.stride = ptrdiff_t(fb_stride);
  s.origin = fb + ptrdiff_t(s.oy) * s.stride + s.ox;
  s.n = uint32_t(clip.n);
  for (size_t i = 0; i < clip.n; ++i)
    s.clip[i] = {clip.r[i].l, clip.r[i].t, clip.r[i].r, clip.r[i].b};
  return DrawStatus::Ok;
}

// Kept for source/API compatibility. There is no draw-burst ownership.
void end_draw() {}

bool take_expose(uint64_t thread_id) {
  SpinlockGuard guard(compositor_lock);
  Window *w = find_window(thread_id);
  if (!w || !w->exposed)
    return false;
  w->exposed = false;
  return true;
}

bool present(uint64_t thread_id, const uint32_t *pixels, uint32_t width,
             uint32_t height, uint32_t pitch) {
  Surface s;
  const DrawStatus st =
      begin_draw(thread_id, true, 0, 0, 0, 0, int(width), int(height), s);
  if (st != DrawStatus::Ok)
    return st != DrawStatus::NoWindow;
  if (width == s.win_w && height == s.win_h && pitch >= width * 4)
    for (uint32_t i = 0; i < s.n; ++i) {
      const int l = s.clip[i].l - s.ox, t = s.clip[i].t - s.oy;
      const int r = s.clip[i].r - s.ox, b = s.clip[i].b - s.oy;
      for (int y = t; y < b; ++y)
        memcpy(s.origin + ptrdiff_t(y) * s.stride + l,
               reinterpret_cast<const uint8_t *>(pixels) + size_t(y) * pitch +
                   size_t(l) * 4,
               size_t(r - l) * 4);
    }
  end_draw();
  return true;
}

bool has_window(uint64_t thread_id) {
  SpinlockGuard guard(compositor_lock);
  return find_window(thread_id) != nullptr;
}

bool title_is_open(const char *title) {
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
  SpinlockGuard guard(compositor_lock);
  if (auto *w = find_window(thread_id)) {
    *x = w->x;
    *y = w->y + TITLE_H;
    return true;
  }
  return false;
}

void run() {
  {
    SpinlockGuard guard(compositor_lock);
    mark_dirty({0, 0, int(screen_w), int(screen_h)});
  }
  for (;;) {
    tick();
    Kernel::Scheduler::thread_sleep_ms(4);
  }
}

} // namespace Kernel::Graphics::WindowManager
