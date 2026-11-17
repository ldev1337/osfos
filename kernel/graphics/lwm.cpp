// lwm.cpp - compositor. Single owner of the window table and of every
// window pixel.
//
// Applications never touch the framebuffer for windowed drawing: each window
// has an SPSC command ring (lwm_draw.h). Every tick this thread
//   1. adopts / closes / resizes windows requested through per-slot mailboxes,
//   2. handles the mouse,
//   3. repaints damaged desktop / chrome,
//   4. drains each window's ring, rasterising clipped to its visible region,
//   5. redraws the software cursor (with a real save-under: the compositor is
//      the only pixel writer, so moving the mouse never damages client area).
//
// No spinlock anywhere. App-side calls are atomic loads/stores or a CAS on a
// slot state. The loop only sleeps on a tick that had nothing at all to do.
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <arch/x86_64/string.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/mouse.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/lwm_draw.h>
#include <kernel/init.h>
#include <kernel/scheduler.h>

namespace Kernel::Graphics::WindowManager {
namespace {

constexpr size_t MAX_WINDOWS = 32, MAX_DAMAGE = 8, kMaxClip = 32;
constexpr int TITLE_H = 26, WINDOW_RADIUS = 12, CORNER = WINDOW_RADIUS - 2;
constexpr uint32_t DESKTOP = 0xFF101216, TITLE_ACTIVE = 0xFF28232F,
                   TITLE_INACTIVE = 0xFF211E29, TEXT = 0xFFF0F5F4,
                   BLACK = 0x00000000, BODY = 0xFF171B20, SHADOW = 0xFF020609;
constexpr uint32_t LIGHTS[3][2] = {{0xFFFF6B67, 0xFFE44845},
                                   {0xFFFFC45C, 0xFFE5A535},
                                   {0xFF5FD39A, 0xFF39A96D}};
constexpr uint8_t SHADOW_LAYER_ALPHA[3] = {12, 22, 30};
constexpr int CURSOR_R = 7, CURSOR_SPAN = 2 * CURSOR_R + 1;
constexpr int kIdleSleepMs = 1; // 0 = spin when idle

template <class T> inline T ld(const T &v) {
  return __atomic_load_n(&v, __ATOMIC_ACQUIRE);
}
template <class T> inline void st(T &v, T x) {
  __atomic_store_n(&v, x, __ATOMIC_RELEASE);
}

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

template <size_t N> struct RectList {
  Rect r[N];
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

// Window = title bar + client; the shadow reaches 8px to the sides, 3px above
// and 22px below.
inline Rect bounds_at(int x, int y, uint32_t w, uint32_t h) {
  return {x - 8, y - 3, x + int(w) + 8, y + TITLE_H + int(h) + 22};
}
inline Rect client_at(int x, int y, uint32_t w, uint32_t h) {
  return {x, y + TITLE_H, x + int(w), y + TITLE_H + int(h)};
}

// ---- state ----------------------------------------------------------------

// Slot life cycle. Apps: FREE -CAS-> CLAIMED -> LIVE -> CLOSING.
// Only the compositor moves CLOSING/LIVE -> FREE.
enum : uint32_t { S_FREE, S_CLAIMED, S_LIVE, S_CLOSING };

struct Window {
  Cmd::Ring ring;
  // written by the registering thread before state = LIVE
  uint64_t thread_id;
  char title[32];
  // app -> compositor mailboxes
  uint32_t state, raise_req;
  uint64_t req_size; // width << 32 | height
  // compositor -> app
  uint32_t exposed;
  int32_t pos_x, pos_y;
  // compositor-private after adoption
  uint32_t width, height;
  int x, y;   // where the window should be
  int sx, sy; // where its pixels currently are (valid if `placed`)
  int tx_off;
  bool known, placed;
  char ctitle[32];
};

Window windows[MAX_WINDOWS]; // zero-initialised: all FREE
size_t z_order[MAX_WINDOWS]; // bottom..top, last = focused
size_t window_count = 0;

uint32_t screen_w = 0, screen_h = 0;
uint32_t *fb = nullptr;
ptrdiff_t fb_stride = 0;
int drag_x = 0, drag_y = 0, cursor_x = -1, cursor_y = -1;
bool dragging = false, old_left = false;
Rect damage[MAX_DAMAGE];
size_t damage_n = 0;
uint8_t g_shadow_alpha[8];
uint64_t g_focus_tid = 0;

struct CursorCell {
  int8_t dx, dy;
  uint32_t color;
};
CursorCell cells[CURSOR_SPAN * CURSOR_SPAN];
uint32_t under[CURSOR_SPAN * CURSOR_SPAN];
size_t ncells = 0;
bool cursor_on = false;
int cur_x = 0, cur_y = 0;

inline uint32_t *fb_row(int y) { return fb + ptrdiff_t(y) * fb_stride; }
inline Rect bounds(const Window &w) {
  return bounds_at(w.x, w.y, w.width, w.height);
}
inline Rect client(const Window &w) {
  return client_at(w.x, w.y, w.width, w.height);
}
inline uint64_t pack(uint32_t w, uint32_t h) { return uint64_t(w) << 32 | h; }

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

void mark_title_dirty(const Window &w) {
  mark_dirty({w.x, w.y, w.x + int(w.width), w.y + TITLE_H});
}

// ---- window table (compositor thread only) --------------------------------

void copy_name(char *dst, const char *src) {
  size_t i = 0;
  for (; src && i < 31 && src[i]; ++i)
    dst[i] = src[i];
  dst[i] = '\0';
}

void layout_title(Window &w) {
  TrueTypeFont *font = Raster::default_font();
  copy_name(w.ctitle, w.title);
  auto tw = [&] {
    return font ? font->get_text_width(13.0f, w.ctitle)
                : int(strlen(w.ctitle) * 8);
  };
  int mw = tw();
  for (size_t len = strlen(w.ctitle); len && mw > int(w.width) - 78; mw = tw())
    w.ctitle[--len] = '\0';
  w.tx_off = std::max(62, (int(w.width) - mw) / 2);
}

void focus_window(size_t idx) {
  size_t *const end = z_order + window_count;
  size_t *const it = std::find(z_order, end, idx);
  if (it == end || it + 1 == end)
    return;
  const Window &w = windows[idx];
  mark_title_dirty(windows[*(end - 1)]);
  mark_title_dirty(w);
  const Rect wb = bounds(w);
  for (size_t *j = it + 1; j != end; ++j) {
    const Rect ab = bounds(windows[*j]);
    if (overlaps(ab, wb))
      mark_dirty(inter(ab, wb));
  }
  std::rotate(it, it + 1, end);
}

void adopt(size_t idx) {
  Window &w = windows[idx];
  if (window_count)
    mark_title_dirty(windows[z_order[window_count - 1]]);
  w.known = true;
  w.placed = false;
  w.sx = w.x;
  w.sy = w.y;
  layout_title(w);
  z_order[window_count++] = idx;
  dragging = false;
  mark_window_dirty(w);
}

void unlink_window(size_t idx) {
  Window &w = windows[idx];
  mark_window_dirty(w);
  size_t *const end = z_order + window_count;
  size_t *const it = std::find(z_order, end, idx);
  if (it == end)
    return;
  if (it + 1 == end)
    dragging = false;
  std::rotate(it, it + 1, end);
  --window_count;
  w.known = false;
  if (window_count)
    mark_title_dirty(windows[z_order[window_count - 1]]);
}

void release_slot(Window &w) {
  w.ring.tail = w.ring.head; // discard anything still queued
  w.exposed = 0;
  w.thread_id = 0;
  w.known = false;
  st(w.state, uint32_t(S_FREE));
}

void apply_resize(Window &w, uint32_t rw, uint32_t rh) {
  mark_window_dirty(w);
  w.width = rw;
  w.height = rh;
  w.placed = false;
  st(w.exposed, 1u);
  w.x = screen_w > rw ? int((screen_w - rw) / 2) : 0;
  w.y = screen_h > rh + TITLE_H + 30
            ? 30 + int((screen_h - 30 - TITLE_H - rh) / 2)
            : 30;
  layout_title(w);
  mark_window_dirty(w);
}

void sync_table() {
  for (size_t i = 0; i < MAX_WINDOWS; ++i) {
    Window &w = windows[i];
    const uint32_t s = ld(w.state);
    if (s == S_LIVE) {
      if (!w.known) {
        adopt(i);
        continue;
      }
      if (__atomic_exchange_n(&w.raise_req, 0u, __ATOMIC_ACQ_REL))
        focus_window(i);
      const uint64_t rs = ld(w.req_size);
      if (uint32_t(rs >> 32) != w.width || uint32_t(rs) != w.height)
        apply_resize(w, uint32_t(rs >> 32), uint32_t(rs));
    } else if (s == S_CLOSING) {
      if (w.known)
        unlink_window(i);
      release_slot(w);
    }
  }
}

void publish() {
  for (size_t z = 0; z < window_count; ++z) {
    Window &w = windows[z_order[z]];
    __atomic_store_n(&w.pos_x, int32_t(w.x), __ATOMIC_RELAXED);
    __atomic_store_n(&w.pos_y, int32_t(w.y + TITLE_H), __ATOMIC_RELAXED);
  }
  __atomic_store_n(&g_focus_tid,
                   window_count ? windows[z_order[window_count - 1]].thread_id
                                : uint64_t(0),
                   __ATOMIC_RELAXED);
}

// ---- software cursor with save-under --------------------------------------

inline int cursor_shape(int dx, int dy) {
  if ((dx == 0 && dy >= -6 && dy < 6) || (dy == 0 && dx >= -6 && dx < 6))
    return 2;
  if ((dx >= -1 && dx <= 1 && dy >= -7 && dy < 7) ||
      (dy >= -1 && dy <= 1 && dx >= -7 && dx < 7))
    return 1;
  return 0;
}

void cursor_hide() {
  if (!cursor_on)
    return;
  for (size_t i = 0; i < ncells; ++i) {
    const int x = cur_x + cells[i].dx, y = cur_y + cells[i].dy;
    if (unsigned(x) < screen_w && unsigned(y) < screen_h)
      fb_row(y)[x] = under[i];
  }
  cursor_on = false;
}

void cursor_show(int cx, int cy) {
  for (size_t i = 0; i < ncells; ++i) {
    const int x = cx + cells[i].dx, y = cy + cells[i].dy;
    if (unsigned(x) < screen_w && unsigned(y) < screen_h) {
      uint32_t &p = fb_row(y)[x];
      under[i] = p;
      p = cells[i].color;
    }
  }
  cur_x = cx;
  cur_y = cy;
  cursor_on = true;
}

// ---- chrome painting ------------------------------------------------------

void paint_shadow(const Canvas &c, const Window &w) {
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

void paint_title(const Canvas &c, const Window &w, bool focused,
                 TrueTypeFont *font) {
  const int x = w.x, y = w.y, ww = int(w.width), body_top = y + TITLE_H;
  if (c.t >= body_top || c.b <= y)
    return;
  Canvas t = c;
  t.b = std::min(c.b, body_top);
  Raster::fill_rounded_rect(t, x, y, ww, TITLE_H + CORNER, CORNER,
                            focused ? TITLE_ACTIVE : TITLE_INACTIVE);
  Raster::fill_rect(t, x, body_top - 1, ww, 1,
                    focused ? 0xFF40515A : 0xFF35434A);
  const int ty = y + 10, tx = x + w.tx_off;
  if (font) {
    Raster::text(t, *font, tx, ty, 13.0f, w.ctitle, TEXT);
  } else {
    const int limit = x + ww - 12;
    const char *s = w.ctitle;
    for (int cx = tx; *s && cx < limit; ++s, cx += 8)
      Raster::glyph8(t, cx, ty, *s, TEXT);
  }
  for (int k = 0; k < 3; ++k) {
    const int cx = x + 18 + 16 * k, cy = y + TITLE_H / 2;
    Raster::fill_circle(t, cx, cy, 5, LIGHTS[k][0]);
    Raster::fill_circle(t, cx, cy, 2, LIGHTS[k][1]);
  }
}

void paint_body(const Canvas &c, const Window &w) {
  const int ww = int(w.width), hh = int(w.height), body_top = w.y + TITLE_H;
  const int rad = std::min(CORNER, std::min(ww, hh) / 2);
  Canvas b = c;
  b.t = std::max(c.t, body_top);
  if (b.t < b.b)
    Raster::fill_rounded_rect(b, w.x, body_top - rad, ww, hh + rad, rad, BODY);
}

void paint_piece(const Rect &p, TrueTypeFont *font) {
  const Canvas c{fb, fb_stride, p.l, p.t, p.r, p.b};
  Raster::fill_rect(c, p.l, p.t, p.r - p.l, p.b - p.t, DESKTOP);
  for (size_t z = 0; z < window_count; ++z) {
    const Window &w = windows[z_order[z]];
    if (!overlaps(bounds(w), p))
      continue;
    paint_shadow(c, w);
    paint_title(c, w, z == window_count - 1, font);
    paint_body(c, w);
  }
}

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

// ---- input ----------------------------------------------------------------

void mouse_update() {
  const bool left = Kernel::Drivers::Mouse::raw_left_pressed();
  const int mx = cursor_x = Kernel::Drivers::Mouse::get_x();
  const int my = cursor_y = Kernel::Drivers::Mouse::get_y();

  if (left && !old_left) {
    for (size_t n = window_count; n--;) {
      const size_t idx = z_order[n];
      Window &w = windows[idx];
      if (mx < w.x - 1 || mx >= w.x + int(w.width) + 1 || my < w.y - 1 ||
          my >= w.y + TITLE_H + int(w.height) + 1)
        continue;
      if (my >= w.y + 5 && my < w.y + TITLE_H - 5 && mx >= w.x + 7 &&
          mx < w.x + 29) {
        const uint64_t tid = w.thread_id;
        unlink_window(idx);
        Kernel::Scheduler::terminate_thread(tid);
        release_slot(w);
      } else {
        focus_window(idx);
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
    Window &w = windows[z_order[window_count - 1]];
    w.x = std::clamp(mx - drag_x, -int(w.width) + 32, int(screen_w) - 32);
    w.y = std::clamp(my - drag_y, 0, int(screen_h) - TITLE_H);
  }

  if (!left)
    dragging = false;
  old_left = left;
}

// ---- planning -------------------------------------------------------------

struct Plan {
  RectList<(MAX_DAMAGE + 4) * 4> pieces;
  bool move = false;
  Rect from{}, to{};
};

void plan(Plan &p) {
  const Rect screen{0, 0, int(screen_w), int(screen_h)};
  RectList<MAX_DAMAGE + 4> dirty;
  for (size_t i = 0; i < damage_n; ++i)
    dirty.add_or_grow(damage[i]);

  // The top window moves by copying its client pixels. Skipped if damage
  // touches the source (those pixels are stale).
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
    if (!stale && !is_empty(to)) {
      p.move = true;
      p.to = to;
      p.from = {to.l - dx, to.t - dy, to.r - dx, to.b - dy};
    }
  }

  for (size_t i = 0; i < dirty.n; ++i)
    p.pieces.add(inter(dirty.r[i], screen));
  if (p.move)
    p.pieces.subtract(p.to);
  damage_n = 0;

  Rect all{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
  for (size_t i = 0; i < p.pieces.n; ++i)
    all = unite(all, p.pieces.r[i]);
  for (size_t z = 0; z < window_count; ++z) {
    Window &w = windows[z_order[z]];
    w.placed = true;
    w.sx = w.x;
    w.sy = w.y;
    if (is_empty(all) || !overlaps(bounds(w), all))
      continue;
    const Rect cl = client(w);
    for (size_t i = 0; i < p.pieces.n; ++i)
      if (overlaps(cl, p.pieces.r[i])) {
        st(w.exposed, 1u);
        break;
      }
  }
}

// ---- command execution ----------------------------------------------------

// Canvases covering the visible part of window z's client area.
size_t visible(size_t z, Canvas *out) {
  const Window &w = windows[z_order[z]];
  const Rect cl = client(w);
  RectList<kMaxClip> clip;
  clip.add(inter(cl, {0, 0, int(screen_w), int(screen_h)}));
  for (size_t j = z + 1; j < window_count && clip.n; ++j) {
    const Rect ab = bounds(windows[z_order[j]]);
    if (overlaps(ab, cl))
      clip.subtract(ab);
  }
  uint32_t *origin = fb + ptrdiff_t(cl.t) * fb_stride + cl.l;
  for (size_t i = 0; i < clip.n; ++i)
    out[i] = {origin,
              fb_stride,
              clip.r[i].l - cl.l,
              clip.r[i].t - cl.t,
              clip.r[i].r - cl.l,
              clip.r[i].b - cl.t};
  return clip.n;
}

bool rings_pending() {
  for (size_t z = 0; z < window_count; ++z) {
    const Cmd::Ring &q = windows[z_order[z]].ring;
    if (q.tail != ld(q.head))
      return true;
  }
  return false;
}

void drain_all() {
  Canvas cv[kMaxClip];
  for (size_t z = 0; z < window_count; ++z) {
    Cmd::Ring &q = windows[z_order[z]].ring;
    uint32_t t = q.tail;
    const uint32_t h = ld(q.head);
    if (t == h)
      continue;
    const size_t n = visible(z, cv);
    while (t != h) {
      const uint32_t *rec = q.buf + (t & Cmd::kMask);
      if ((rec[0] & 0xFFu) != Cmd::kPad)
        for (size_t i = 0; i < n; ++i)
          Raster::execute(cv[i], rec);
      t += rec[0] >> 8;
    }
    st(q.tail, t);
  }
}

bool tick() {
  sync_table();
  mouse_update();
  publish();

  Plan p;
  plan(p);
  const bool moved = !cursor_on || cursor_x != cur_x || cursor_y != cur_y;
  if (!p.pieces.n && !p.move && !moved && !rings_pending())
    return false;

  cursor_hide();
  if (p.move)
    move_pixels(p.from, p.to);
  TrueTypeFont *font = Raster::default_font();
  for (size_t i = 0; i < p.pieces.n; ++i)
    paint_piece(p.pieces.r[i], font);
  drain_all();
  cursor_show(cursor_x, cursor_y);
  return true;
}

Window *find_live(uint64_t tid) {
  if (tid)
    for (Window &w : windows)
      if (ld(w.state) == S_LIVE && w.thread_id == tid)
        return &w;
  return nullptr;
}

} // namespace

// ---- public API -----------------------------------------------------------

bool __init initialize() {
  if (!Kernel::Drivers::Framebuffer::valid())
    return false;
  const Canvas s = Raster::screen();
  fb = s.origin;
  fb_stride = s.stride;
  screen_w = uint32_t(s.r);
  screen_h = uint32_t(s.b);
  if (screen_w < 240 || screen_h < 160 || !fb ||
      fb_stride < ptrdiff_t(screen_w))
    return false;

  Raster::default_font();
  for (unsigned m = 0; m < 8; ++m) {
    unsigned inv = 255;
    for (int k = 0; k < 3; ++k)
      if (m & (1u << k))
        inv = inv * (255 - SHADOW_LAYER_ALPHA[k]) / 255;
    g_shadow_alpha[m] = uint8_t(255 - inv);
  }
  ncells = 0;
  for (int dy = -CURSOR_R; dy <= CURSOR_R; ++dy)
    for (int dx = -CURSOR_R; dx <= CURSOR_R; ++dx)
      if (const int sh = cursor_shape(dx, dy))
        cells[ncells++] = {int8_t(dx), int8_t(dy), sh == 1 ? BLACK : TEXT};
  return true;
}

int register_window(uint64_t thread_id, const char *title, bool launcher,
                    uint32_t width, uint32_t height) {
  if (!fb || !thread_id)
    return -1;
  if (Window *w = find_live(thread_id)) {
    st(w->raise_req, 1u);
    return int(w - windows);
  }
  for (size_t slot = 0; slot < MAX_WINDOWS; ++slot) {
    Window &w = windows[slot];
    uint32_t expect = S_FREE;
    if (ld(w.state) != S_FREE ||
        !__atomic_compare_exchange_n(&w.state, &expect, uint32_t(S_CLAIMED),
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      continue;

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

    w.thread_id = thread_id;
    copy_name(w.title, title);
    w.width = width;
    w.height = height;
    w.x = w.sx = x;
    w.y = w.sy = y;
    w.req_size = pack(width, height);
    w.raise_req = 0;
    w.exposed = 1;
    w.pos_x = x;
    w.pos_y = y + TITLE_H;
    w.known = w.placed = false;
    st(w.state, uint32_t(S_LIVE));
    return int(slot);
  }
  return -1;
}

bool request_window_size(uint64_t thread_id, uint32_t rw, uint32_t rh,
                         uint32_t *actual_width, uint32_t *actual_height) {
  Window *w = find_live(thread_id);
  if (!w) {
    if (register_window(thread_id, "App", false, rw, rh) < 0 ||
        !(w = find_live(thread_id)))
      return false;
  } else if (rw && rh) {
    st(w->req_size, pack(rw, rh));
  }
  const uint64_t rs = ld(w->req_size);
  *actual_width = uint32_t(rs >> 32);
  *actual_height = uint32_t(rs);
  return true;
}

void unregister_window(uint64_t thread_id) {
  if (Window *w = find_live(thread_id))
    st(w->state, uint32_t(S_CLOSING));
}

// Queues a whole-window blit. Large sources are read by reference.
bool present(uint64_t thread_id, const uint32_t *pixels, uint32_t width,
             uint32_t height, uint32_t pitch) {
  Window *w = find_live(thread_id);
  if (!w)
    return false;
  const uint64_t rs = ld(w->req_size);
  if (width == uint32_t(rs >> 32) && height == uint32_t(rs) &&
      pitch >= width * 4)
    Cmd::blit(&w->ring, 0, 0, width, height, pixels, pitch, 0, 0);
  return true;
}

Cmd::Ring *queue_of(uint64_t thread_id) {
  Window *w = find_live(thread_id);
  return w ? &w->ring : nullptr;
}

bool take_expose(uint64_t thread_id) {
  Window *w = find_live(thread_id);
  return w && __atomic_exchange_n(&w->exposed, 0u, __ATOMIC_ACQ_REL);
}

bool has_window(uint64_t thread_id) { return find_live(thread_id) != nullptr; }

bool title_is_open(const char *title) {
  for (const Window &w : windows) {
    if (ld(w.state) != S_LIVE)
      continue;
    size_t i = 0;
    while (i < 31 && w.title[i] && w.title[i] == title[i])
      ++i;
    if (!w.title[i] && !title[i])
      return true;
  }
  return false;
}

uint64_t keyboard_focus_thread_id() {
  return __atomic_load_n(&g_focus_tid, __ATOMIC_RELAXED);
}

bool has_keyboard_focus(uint64_t thread_id) {
  return thread_id && keyboard_focus_thread_id() == thread_id;
}

bool get_window_position(uint64_t thread_id, int *x, int *y) {
  if (const Window *w = find_live(thread_id)) {
    *x = __atomic_load_n(&w->pos_x, __ATOMIC_RELAXED);
    *y = __atomic_load_n(&w->pos_y, __ATOMIC_RELAXED);
    return true;
  }
  return false;
}

void run() {
  mark_dirty({0, 0, int(screen_w), int(screen_h)});
  for (;;) {
    if (tick())
      continue;
    if constexpr (kIdleSleepMs > 0)
      Kernel::Scheduler::thread_sleep_ms(kIdleSleepMs);
    else
      asm volatile("pause");
  }
}

} // namespace Kernel::Graphics::WindowManager
