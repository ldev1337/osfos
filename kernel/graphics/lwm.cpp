// lwm.cpp - compositor. Single owner of the window table and of every
// window pixel.
//
// Applications never touch the framebuffer for windowed drawing: each window
// has an SPSC command ring (lwm_draw.h). Every tick this thread
//   1. adopts / closes / resizes windows requested through per-slot mailboxes,
//   2. handles the mouse,
//   3. repaints damaged desktop / chrome (tile-composed, one write per pixel),
//   4. drains each window's ring, rasterising clipped to its exact visible
//      shape (rounded corners respected, shadows of windows above applied),
//   5. redraws the software cursor (real save-under, only lifted when the
//      frame really writes underneath it).
//
// Memory: there is NO back buffer. The only scratch is one 512x16 tile
// (32 KiB) + a 8 KiB change mask, reused for every tile.
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

// ---- look -----------------------------------------------------------------

constexpr size_t MAX_WINDOWS = 32, kMaxDamage = 32, kMaxClip = 96,
                 kMaxPieces = 256;
constexpr int TITLE_H = 26, CORNER = 10;

constexpr uint32_t DESKTOP = 0xFF101216, TITLE_BG = 0xFF28232F,
                   TEXT = 0xFFF0F5F4, TEXT_DIM = 0xFF8D969C, BLACK = 0x00000000,
                   BODY = 0xFF171B20, SHADOW = 0xFF020609,
                   LINE_ACTIVE = 0xFF40515A, LINE_INACTIVE = 0xFF35434A;
constexpr uint32_t LIGHTS[3][2] = {{0xFFFF6B67, 0xFFE44845},
                                   {0xFFFFC45C, 0xFFE5A535},
                                   {0xFF5FD39A, 0xFF39A96D}};
constexpr uint32_t LIGHT_OFF[2] = {0xFF444A52, 0xFF343A41};

// Two stacked soft shadows (wide ambient + tight contact). Each one is the
// exact blur of a rectangle: alpha(x,y) = A * cdf(x) * cdf(y), where cdf is a
// smootherstep ramp of width 2k. Rect = window inset by `inset`, moved down
// by `dy`.
struct ShadowSpec {
  int k, inset, dy;
  uint32_t alpha; // 0..256
};
constexpr ShadowSpec kShadow[2] = {{20, 4, 10, 160}, {5, 0, 2, 110}};
constexpr int kMaxK = 20;
constexpr int cmax(int a, int b) { return a > b ? a : b; }
// How far the shadows reach outside the window rectangle.
constexpr int EXT_X =
    cmax(kShadow[0].k - kShadow[0].inset, kShadow[1].k - kShadow[1].inset);
constexpr int EXT_T = cmax(kShadow[0].k - kShadow[0].inset - kShadow[0].dy,
                           kShadow[1].k - kShadow[1].inset - kShadow[1].dy);
constexpr int EXT_B = cmax(kShadow[0].k - kShadow[0].inset + kShadow[0].dy,
                           kShadow[1].k - kShadow[1].inset + kShadow[1].dy);

constexpr int CURSOR_R = 7, CURSOR_SPAN = 2 * CURSOR_R + 1;
constexpr int kIdleSleepMs = 1; // 0 = spin when idle

// Compose tile: small enough to live in L1, big enough to amortise setup.
constexpr int kTileW = 512, kTileH = 16;
constexpr size_t kTilePx = size_t(kTileW) * kTileH;

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
inline bool contains(const Rect &a, const Rect &b) {
  return a.l <= b.l && a.t <= b.t && a.r >= b.r && a.b >= b.b;
}
inline Rect unite(const Rect &a, const Rect &b) {
  return {std::min(a.l, b.l), std::min(a.t, b.t), std::max(a.r, b.r),
          std::max(a.b, b.b)};
}
inline Rect inter(const Rect &a, const Rect &b) {
  return {std::max(a.l, b.l), std::max(a.t, b.t), std::min(a.r, b.r),
          std::min(a.b, b.b)};
}
inline int64_t area(const Rect &r) {
  return is_empty(r) ? 0 : int64_t(r.r - r.l) * int64_t(r.b - r.t);
}

template <size_t N> struct RectList {
  Rect r[N];
  size_t n = 0;
  bool ovf = false; // an add() was dropped: callers that care fall back

  void add(const Rect &x) {
    if (is_empty(x))
      return;
    if (n < N)
      r[n++] = x;
    else
      ovf = true;
  }
  void subtract(const Rect &h) {
    RectList out;
    for (size_t i = 0; i < n; ++i) {
      const Rect &a = r[i];
      if (!overlaps(a, h)) {
        out.add(a);
        continue;
      }
      const int mt = a.t > h.t ? a.t : h.t;
      const int mb = a.b < h.b ? a.b : h.b;
      if (a.t < h.t)
        out.add({a.l, a.t, a.r, h.t});
      if (a.b > h.b)
        out.add({a.l, h.b, a.r, a.b});
      if (a.l < h.l)
        out.add({a.l, mt, h.l, mb});
      if (a.r > h.r)
        out.add({h.r, mt, a.r, mb});
    }
    n = out.n;
    ovf = ovf || out.ovf;
    for (size_t i = 0; i < n; ++i)
      r[i] = out.r[i];
  }
};

// The window rectangle (title bar + client) and everything it can darken.
inline Rect body_at(int x, int y, uint32_t w, uint32_t h) {
  return {x, y, x + int(w), y + TITLE_H + int(h)};
}
inline Rect ext_at(int x, int y, uint32_t w, uint32_t h) {
  return {x - EXT_X, y - EXT_T, x + int(w) + EXT_X,
          y + TITLE_H + int(h) + EXT_B};
}
inline Rect client_at(int x, int y, uint32_t w, uint32_t h) {
  return {x, y + TITLE_H, x + int(w), y + TITLE_H + int(h)};
}

// ---- state ----------------------------------------------------------------

// Slot life cycle. Apps: FREE -CAS-> CLAIMED -> LIVE -> CLOSING.
// Only the compositor moves CLOSING/LIVE -> FREE.
enum : uint32_t { S_FREE, S_CLAIMED, S_LIVE, S_CLOSING };

// Slot shared with app threads, split by who touches which cache line so a
// write from one side never invalidates a line the other side is spinning on:
//   line 0  polled by the compositor every tick (sync_table): app-written
//           mailboxes plus the rarely-changing size/known flag
//   line 1  compositor -> app results (written by publish/plan)
//   line 2+ cold: spawn position and titles, touched on adopt/resize/paint
//   last    the command ring, on its own line(s), never shared with the rest
struct alignas(64) Window {
  // --- line 0 ---------------------------------------------------------------
  uint32_t state;     // S_* life cycle (CAS by apps, reset by compositor)
  uint32_t raise_req; // app -> compositor
  uint64_t req_size;  // width << 32 | height, app -> compositor
  uint64_t thread_id; // written by the registering thread before state = LIVE
  uint32_t width, height; // size last applied (compared against req_size)
  bool known;             // linked into zg[]
  // --- line 1 ---------------------------------------------------------------
  alignas(64) uint32_t exposed;
  int32_t pos_x, pos_y;
  // --- cold -----------------------------------------------------------------
  alignas(64) int init_x, init_y; // spawn position, consumed by adopt()
  char title[32];
  char ctitle[32];
  // --- ring -----------------------------------------------------------------
  alignas(64) Cmd::Ring ring;
};

// Hot, compositor-private geometry. Packed in z-order (bottom..top, last =
// focused) so every z walk is a linear scan of 32-byte records, two per cache
// line, and never touches the app-written Window lines (no false sharing).
struct alignas(32) Geo {
  int x, y;   // where the window should be
  int sx, sy; // where its pixels currently are (valid if `placed`)
  uint32_t width, height;
  int tx_off;
  uint8_t slot; // index into windows[]
  bool placed;
};
static_assert(sizeof(Geo) == 32, "Geo must stay two per cache line");

alignas(64) Window windows[MAX_WINDOWS]; // zero-initialised: all FREE
alignas(64) Geo zg[MAX_WINDOWS];         // bottom..top, last = focused
size_t window_count = 0;
alignas(
    64) uint32_t g_heads[MAX_WINDOWS]; // ring heads snapshotted once per tick

uint32_t screen_w = 0, screen_h = 0;
uint32_t *fb = nullptr;
ptrdiff_t fb_stride = 0;
int drag_x = 0, drag_y = 0, cursor_x = -1, cursor_y = -1;
bool dragging = false, old_left = false;
alignas(64) Rect damage[kMaxDamage];
size_t damage_n = 0;
uint64_t g_focus_tid = 0;

struct CursorSpan {
  int8_t dy, x0, x1;
  uint32_t color;
  size_t offset;
};
CursorSpan cursor_spans[CURSOR_SPAN * 3];
uint32_t under[CURSOR_SPAN * CURSOR_SPAN];
size_t nspans = 0, ncells = 0;
bool cursor_on = false;
int cur_x = 0, cur_y = 0;

alignas(64) uint32_t scratch[kTilePx];
alignas(64) uint8_t changed[kTilePx];

inline uint32_t *fb_row(int y) { return fb + ptrdiff_t(y) * fb_stride; }
inline Rect body(const Geo &w) { return body_at(w.x, w.y, w.width, w.height); }
inline Rect ext(const Geo &w) { return ext_at(w.x, w.y, w.width, w.height); }
inline Rect client(const Geo &w) {
  return client_at(w.x, w.y, w.width, w.height);
}
inline uint64_t pack(uint32_t w, uint32_t h) { return uint64_t(w) << 32 | h; }

// ---- rounded corners (analytic coverage, no libm) --------------------------

// g_cov[R][k][dx]: coverage (0..255) of the pixel k rows / dx columns in from
// the outer corner of a corner of radius R. g_ins[R][k]: first dx that is
// fully opaque. These two tables are the single source of truth for the
// window shape: painting, shadow masking, clipping and moving all use them.
uint8_t g_cov[CORNER + 1][CORNER][CORNER];
uint8_t g_ins[CORNER + 1][CORNER];

uint32_t isqrt32(uint32_t n) {
  if (n <= 1)
    return n;
  int leading_zeros = __builtin_clz(n);
  int shift = 30 - ((30 - leading_zeros) & ~1); // Nearest even bit <= msb

  uint32_t bit = 1u << shift;
  uint32_t r = 0;

  while (bit) {
    if (n >= r + bit) {
      n -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return r;
}

void init_corners() {
  for (int R = 1; R <= CORNER; ++R)
    for (int k = 0; k < R; ++k) {
      int ins = R;
      for (int dx = 0; dx < R; ++dx) {
        const int a = 2 * R - 2 * dx - 1, b = 2 * R - 2 * k - 1;
        const int df = int(isqrt32(uint32_t(64 * (a * a + b * b))));
        int cf = R * 16 - df + 8; // coverage in 1/16
        cf = cf < 0 ? 0 : (cf > 16 ? 16 : cf);
        const int cov = (cf * 255 + 8) / 16;
        g_cov[R][k][dx] = uint8_t(cov);
        if (cov == 255 && ins == R)
          ins = dx;
      }
      g_ins[R][k] = uint8_t(ins);
    }
}

inline int top_radius(const Geo &w) {
  return std::min(CORNER, int(w.width) / 2);
}
inline int bottom_radius(const Geo &w) {
  return std::min(CORNER, int(std::min(w.width, w.height)) / 2);
}

// Fully opaque span of window row `py` (title bar + client, rounded corners).
inline bool opaque_span(const Geo &w, int py, int &a, int &b) {
  const int H = TITLE_H + int(w.height), dy = py - w.y;
  if (dy < 0 || dy >= H)
    return false;
  const int rt = top_radius(w), rb = bottom_radius(w);
  int ins = 0;
  if (dy < rt)
    ins = g_ins[rt][dy];
  else if (dy >= H - rb)
    ins = g_ins[rb][H - 1 - dy];
  a = w.x + ins;
  b = w.x + int(w.width) - ins;
  return a < b;
}

// Fully opaque pixels as a handful of row-merged rects. client_only: skip the
// title bar (what applications may draw into).
template <size_t N>
void opaque_rects(const Geo &w, RectList<N> &out, bool client_only) {
  const int x = w.x, r = x + int(w.width), H = TITLE_H + int(w.height);
  const int rt = top_radius(w), rb = bottom_radius(w);
  const int first = client_only ? TITLE_H : 0;
  auto rows = [&](int ra, int re, int ins) {
    ra = std::max(ra, first);
    if (ra < re)
      out.add({x + ins, w.y + ra, r - ins, w.y + re});
  };
  for (int dy = 0; dy < rt;) {
    const int ins = g_ins[rt][dy];
    int e = dy + 1;
    while (e < rt && g_ins[rt][e] == ins)
      ++e;
    rows(dy, e, ins);
    dy = e;
  }
  rows(rt, H - rb, 0);
  for (int k = 0; k < rb;) {
    const int ins = g_ins[rb][k];
    int e = k + 1;
    while (e < rb && g_ins[rb][e] == ins)
      ++e;
    rows(H - e, H - k, ins);
    k = e;
  }
}

// ---- soft shadow ------------------------------------------------------------

struct ShadowLayer {
  int k, inset, dy;
  uint32_t alpha;
  uint16_t cdf[2 * kMaxK];
};
ShadowLayer g_layers[2];

void init_shadows() {
  for (int i = 0; i < 2; ++i) {
    ShadowLayer &L = g_layers[i];
    L.k = kShadow[i].k;
    L.inset = kShadow[i].inset;
    L.dy = kShadow[i].dy;
    L.alpha = kShadow[i].alpha;
    for (int d = -L.k; d < L.k; ++d) {
      // pixel centre, 0..1 across the ramp, 16.16 fixed point
      const int64_t t = int64_t(2 * d + 1 + 2 * L.k) * 65536 / (4 * L.k);
      const int64_t t2 = (t * t) >> 16, t3 = (t2 * t) >> 16;
      const int64_t inner = (((6 * t - 15 * 65536) * t) >> 16) + 10 * 65536;
      const int64_t s = (t3 * inner) >> 16; // smootherstep
      L.cdf[d + L.k] = uint16_t((s * 256 + 32768) >> 16);
    }
  }
}

inline int prof(const ShadowLayer &L, int d) {
  if (d < -L.k)
    return 0;
  if (d >= L.k)
    return 256;
  return L.cdf[d + L.k];
}

// d * (256-a)/256 + s * a/256 per channel, a in 0..256, result opaque.
inline uint32_t mix(uint32_t d, uint32_t s, uint32_t a) {
  const uint32_t ia = 256 - a;
  const uint32_t rb =
      (((d & 0x00FF00FFu) * ia + (s & 0x00FF00FFu) * a) >> 8) & 0x00FF00FFu;
  const uint32_t g =
      (((d & 0x0000FF00u) * ia + (s & 0x0000FF00u) * a) >> 8) & 0x0000FF00u;
  return 0xFF000000u | rb | g;
}

void shade_generic(uint32_t *row, const ShadowLayer &L, uint32_t rowA, int sl,
                   int sr, int a, int b) {
  for (int px = a; px < b; ++px) {
    const int fx = prof(L, px - sl) + prof(L, sr - 1 - px) - 256;
    if (fx <= 0)
      continue;
    const uint32_t al = (uint32_t(fx) * rowA + 32768u) >> 16;
    if (al)
      row[px] = mix(row[px], SHADOW, al);
  }
}

void shade_flat(uint32_t *row, uint32_t rowA, int a, int b) {
  const uint32_t al = (rowA + 128u) >> 8;
  if (!al)
    return;
  for (int px = a; px < b; ++px)
    row[px] = mix(row[px], SHADOW, al);
}

void shade_run(uint32_t *row, const ShadowLayer &L, uint32_t rowA, int sl,
               int sr, int a, int b) {
  if (a >= b)
    return;
  const int f0 = sl + L.k, f1 = sr - L.k; // fx == 256 on [f0, f1)
  if (f0 < f1) {
    const int m0 = std::clamp(f0, a, b), m1 = std::clamp(f1, a, b);
    shade_generic(row, L, rowA, sl, sr, a, m0);
    shade_flat(row, rowA, m0, m1);
    shade_generic(row, L, rowA, sl, sr, m1, b);
  } else {
    shade_generic(row, L, rowA, sl, sr, a, b);
  }
}

// Darkens canvas pixels around (never under) the opaque window shape.
void paint_shadow(const Canvas &c, const Geo &w) {
  const int H = TITLE_H + int(w.height);
  for (const ShadowLayer &L : g_layers) {
    const int sl = w.x + L.inset, sr = w.x + int(w.width) - L.inset;
    const int st = w.y + L.inset + L.dy, sb = w.y + H - L.inset + L.dy;
    if (sr <= sl || sb <= st)
      continue;
    const int y0 = std::max(c.t, st - L.k), y1 = std::min(c.b, sb + L.k);
    const int xa = std::max(c.l, sl - L.k), xb = std::min(c.r, sr + L.k);
    if (y0 >= y1 || xa >= xb)
      continue;
    for (int py = y0; py < y1; ++py) {
      const int fy = prof(L, py - st) + prof(L, sb - 1 - py) - 256;
      if (fy <= 0)
        continue;
      const uint32_t rowA = uint32_t(fy) * L.alpha;
      uint32_t *row = c.origin + ptrdiff_t(py) * c.stride;
      int oa, ob;
      if (!opaque_span(w, py, oa, ob))
        oa = ob = xb;
      shade_run(row, L, rowA, sl, sr, xa, std::min(xb, oa));
      shade_run(row, L, rowA, sl, sr, std::max(xa, ob), xb);
    }
  }
}

// ---- state mutation helpers -------------------------------------------------

void mark_dirty(const Rect &r) {
  if (is_empty(r))
    return;
  for (size_t i = 0; i < damage_n; ++i)
    if (contains(damage[i], r))
      return;
  for (size_t i = 0; i < damage_n;) {
    if (contains(r, damage[i]))
      damage[i] = damage[--damage_n];
    else
      ++i;
  }
  if (damage_n < kMaxDamage) {
    damage[damage_n++] = r;
    return;
  }
  size_t best = 0;
  int64_t gain = INT64_MAX;
  for (size_t i = 0; i < damage_n; ++i) {
    const int64_t g = area(unite(damage[i], r)) - area(damage[i]);
    if (g < gain) {
      gain = g;
      best = i;
    }
  }
  damage[best] = unite(damage[best], r);
}

void mark_window_dirty(const Geo &w) {
  mark_dirty(ext(w));
  if (w.placed && (w.sx != w.x || w.sy != w.y))
    mark_dirty(ext_at(w.sx, w.sy, w.width, w.height));
}

// Focus only changes the title's text, lights and rule: repaint the opaque
// interior of the bar and leave the rounded corners (which show whatever is
// beneath) alone.
void mark_title_dirty(const Geo &w) {
  const int rt = top_radius(w), r = w.x + int(w.width);
  mark_dirty({w.x + rt, w.y, r - rt, w.y + TITLE_H});
  mark_dirty({w.x, w.y + rt, w.x + rt, w.y + TITLE_H});
  mark_dirty({r - rt, w.y + rt, r, w.y + TITLE_H});
}

// ---- window table (compositor thread only) --------------------------------

void copy_name(char *dst, const char *src) {
  size_t i = 0;
  for (; src && i < 31 && src[i]; ++i)
    dst[i] = src[i];
  dst[i] = '\0';
}

void layout_title(Window &w, Geo &g) {
  TrueTypeFont *font = Raster::default_font();
  copy_name(w.ctitle, w.title);
  auto tw = [&] {
    return font ? font->get_text_width(13.0f, w.ctitle)
                : int(strlen(w.ctitle) * 8);
  };
  int mw = tw();
  for (size_t len = strlen(w.ctitle); len && mw > int(g.width) - 78; mw = tw())
    w.ctitle[--len] = '\0';
  g.tx_off = std::max(62, (int(g.width) - mw) / 2);
}

// z position of a slot (compositor thread only; <= 32 entries).
inline Geo *geo_of(size_t slot) {
  for (size_t z = 0; z < window_count; ++z)
    if (zg[z].slot == slot)
      return &zg[z];
  return nullptr;
}

void focus_window(size_t idx) {
  Geo *const end = zg + window_count;
  Geo *it = zg;
  while (it != end && it->slot != idx)
    ++it;
  if (it == end || it + 1 == end)
    return;
  mark_title_dirty(*(end - 1));
  mark_title_dirty(*it);
  // Only pixels where the stacking order of two windows flips change:
  // a's shadow over w's body and a's body over w's shadow. Shadow-over-shadow
  // is commutative and body pixels of w that nobody covers keep their client.
  const Rect wb = body(*it), we = ext(*it);
  for (Geo *j = it + 1; j != end; ++j) {
    const Rect ab = body(*j), ae = ext(*j);
    if (overlaps(ae, wb))
      mark_dirty(inter(ae, wb));
    if (overlaps(ab, we))
      mark_dirty(inter(ab, we));
  }
  std::rotate(it, it + 1, end);
}

void adopt(size_t idx) {
  Window &w = windows[idx];
  if (window_count)
    mark_title_dirty(zg[window_count - 1]);
  w.known = true;
  Geo &g = zg[window_count++];
  g = Geo{};
  g.x = g.sx = w.init_x;
  g.y = g.sy = w.init_y;
  g.width = w.width;
  g.height = w.height;
  g.slot = uint8_t(idx);
  g.placed = false;
  layout_title(w, g);
  dragging = false;
  mark_window_dirty(g);
}

void unlink_window(size_t idx) {
  Window &w = windows[idx];
  Geo *const end = zg + window_count;
  Geo *it = zg;
  while (it != end && it->slot != idx)
    ++it;
  if (it == end)
    return;
  mark_window_dirty(*it);
  if (it + 1 == end)
    dragging = false;
  std::rotate(it, it + 1, end);
  --window_count;
  w.known = false;
  if (window_count)
    mark_title_dirty(zg[window_count - 1]);
}

void release_slot(Window &w) {
  w.ring.tail = ld(w.ring.head); // discard anything still queued
  w.exposed = 0;
  w.thread_id = 0;
  w.known = false;
  st(w.state, uint32_t(S_FREE));
}

void apply_resize(Window &w, Geo &g, uint32_t rw, uint32_t rh) {
  mark_window_dirty(g);
  w.width = g.width = rw;
  w.height = g.height = rh;
  g.placed = false;
  st(w.exposed, 1u);
  g.x = screen_w > rw ? int((screen_w - rw) / 2) : 0;
  g.y = screen_h > rh + TITLE_H + 30
            ? 30 + int((screen_h - 30 - TITLE_H - rh) / 2)
            : 30;
  layout_title(w, g);
  mark_window_dirty(g);
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
        if (Geo *g = geo_of(i))
          apply_resize(w, *g, uint32_t(rs >> 32), uint32_t(rs));
    } else if (s == S_CLOSING) {
      if (w.known)
        unlink_window(i);
      release_slot(w);
    }
  }
}

void publish() {
  for (size_t z = 0; z < window_count; ++z) {
    const Geo &g = zg[z];
    Window &w = windows[g.slot];
    // Only write when changed: an idle window's line stays Shared in the
    // app's cache instead of bouncing every tick.
    const int32_t px = int32_t(g.x), py = int32_t(g.y + TITLE_H);
    if (__atomic_load_n(&w.pos_x, __ATOMIC_RELAXED) != px)
      __atomic_store_n(&w.pos_x, px, __ATOMIC_RELAXED);
    if (__atomic_load_n(&w.pos_y, __ATOMIC_RELAXED) != py)
      __atomic_store_n(&w.pos_y, py, __ATOMIC_RELAXED);
  }
  __atomic_store_n(&g_focus_tid,
                   window_count ? windows[zg[window_count - 1].slot].thread_id
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

inline Rect cursor_box() {
  return {cur_x - CURSOR_R, cur_y - CURSOR_R, cur_x + CURSOR_R + 1,
          cur_y + CURSOR_R + 1};
}

void cursor_hide() {
  if (!cursor_on)
    return;
  for (size_t i = 0; i < nspans; ++i) {
    const CursorSpan &s = cursor_spans[i];
    const int y = cur_y + s.dy;
    if (unsigned(y) >= screen_h)
      continue;
    const int ox = cur_x + s.x0;
    const int x0 = std::max(0, ox);
    const int x1 = std::min(int(screen_w), cur_x + int(s.x1) + 1);
    if (x0 >= x1)
      continue;
    memcpy(fb_row(y) + x0, under + s.offset + size_t(x0 - ox),
           size_t(x1 - x0) * 4);
  }
  cursor_on = false;
}

void cursor_show(int cx, int cy) {
  for (size_t i = 0; i < nspans; ++i) {
    const CursorSpan &s = cursor_spans[i];
    const int y = cy + s.dy;
    if (unsigned(y) >= screen_h)
      continue;
    const int ox = cx + s.x0;
    const int x0 = std::max(0, ox);
    const int x1 = std::min(int(screen_w), cx + int(s.x1) + 1);
    if (x0 >= x1)
      continue;
    uint32_t *row = fb_row(y) + x0;
    uint32_t *save = under + s.offset + size_t(x0 - ox);
    const size_t n = size_t(x1 - x0);
    memcpy(save, row, n * 4);
    for (size_t j = 0; j < n; ++j)
      row[j] = s.color;
  }
  cur_x = cx;
  cur_y = cy;
  cursor_on = true;
}

// ---- chrome painting (onto any canvas: tile scratch, never the screen) ----

inline void fill_span(const Canvas &c, int py, int a, int b, uint32_t color) {
  a = std::max(a, c.l);
  b = std::min(b, c.r);
  if (py < c.t || py >= c.b || a >= b)
    return;
  uint32_t *row = c.origin + ptrdiff_t(py) * c.stride;
  for (int px = a; px < b; ++px)
    row[px] = color;
}

// Anti-aliased pixels of one corner row (both sides), blended over whatever is
// already on the canvas.
inline void edge_row(const Canvas &c, const Geo &w, int py, int R, int k,
                     uint32_t color) {
  if (py < c.t || py >= c.b)
    return;
  uint32_t *row = c.origin + ptrdiff_t(py) * c.stride;
  const int ins = g_ins[R][k], rx = w.x + int(w.width) - 1;
  for (int dx = 0; dx < ins; ++dx) {
    const uint32_t cov = g_cov[R][k][dx];
    if (!cov)
      continue;
    const uint32_t a = cov + (cov >> 7);
    const int lx = w.x + dx, mx = rx - dx;
    if (lx >= c.l && lx < c.r)
      row[lx] = mix(row[lx], color, a);
    if (mx >= c.l && mx < c.r)
      row[mx] = mix(row[mx], color, a);
  }
}

// Only the anti-aliased corner pixels (used to re-seat a window's silhouette
// over client pixels that apps just drew underneath it).
void paint_edges(const Canvas &c, const Geo &w) {
  const int H = TITLE_H + int(w.height), rt = top_radius(w),
            rb = bottom_radius(w);
  for (int k = 0; k < rt; ++k)
    edge_row(c, w, w.y + k, rt, k, TITLE_BG);
  for (int k = 0; k < rb; ++k)
    edge_row(c, w, w.y + H - 1 - k, rb, k, BODY);
}

void paint_title(const Canvas &c, const Geo &w, const char *ctitle,
                 bool focused, TrueTypeFont *font) {
  const int x = w.x, y = w.y, ww = int(w.width), r = x + ww,
            body_top = y + TITLE_H;
  if (c.t >= body_top || c.b <= y || c.l >= r || c.r <= x)
    return;
  const int rt = top_radius(w);
  const int y0 = std::max(c.t, y), y1 = std::min(c.b, body_top);
  for (int py = y0; py < y1; ++py) {
    const int dy = py - y, ins = dy < rt ? g_ins[rt][dy] : 0;
    fill_span(c, py, x + ins, r - ins, TITLE_BG);
    if (dy < rt)
      edge_row(c, w, py, rt, dy, TITLE_BG);
  }
  fill_span(c, body_top - 1, x, r, focused ? LINE_ACTIVE : LINE_INACTIVE);

  Canvas t = c;
  t.t = y0;
  t.b = y1;
  const uint32_t tc = focused ? TEXT : TEXT_DIM;
  const int ty = y + 7, tx = x + w.tx_off;
  if (font) {
    Raster::text(t, *font, tx, ty, 14.0f, ctitle, tc);
  } else {
    const int limit = x + ww - 12;
    const char *s = ctitle;
    for (int cx = tx; *s && cx < limit; ++s, cx += 8)
      Raster::glyph8(t, cx, ty, *s, tc);
  }
  for (int k = 0; k < 3; ++k) {
    const int cx = x + 18 + 16 * k, cy = y + TITLE_H / 2;
    if (cx + 6 <= c.l || cx - 5 >= c.r)
      continue;
    Raster::fill_circle(t, cx, cy, 5, focused ? LIGHTS[k][0] : LIGHT_OFF[0]);
    Raster::fill_circle(t, cx, cy, 2, focused ? LIGHTS[k][1] : LIGHT_OFF[1]);
  }
}

void paint_body(const Canvas &c, const Geo &w) {
  const int x = w.x, r = x + int(w.width), body_top = w.y + TITLE_H,
            bottom = body_top + int(w.height), rb = bottom_radius(w);
  const int y0 = std::max(c.t, body_top), y1 = std::min(c.b, bottom);
  for (int py = y0; py < y1; ++py) {
    const int rr = bottom - 1 - py, ins = rr < rb ? g_ins[rb][rr] : 0;
    fill_span(c, py, x + ins, r - ins, BODY);
    if (rr < rb)
      edge_row(c, w, py, rb, rr, BODY);
  }
}

// One tile = desktop + every window bottom-up, composed in cache, then pushed
// to the framebuffer with a single sequential write per row. The screen never
// shows an intermediate state and the framebuffer is never read.
void compose_tile(const Rect &T, TrueTypeFont *font) {
  const int pw = T.r - T.l, ph = T.b - T.t;
  const Canvas c{scratch - ptrdiff_t(T.t) * pw - T.l, pw, T.l, T.t, T.r, T.b};
  for (size_t i = 0, n = size_t(pw) * size_t(ph); i < n; ++i)
    scratch[i] = DESKTOP;
  for (size_t z = 0; z < window_count; ++z) {
    const Geo &w = zg[z];
    if (!overlaps(ext(w), T))
      continue;
    paint_shadow(c, w);
    paint_title(c, w, windows[w.slot].ctitle, z == window_count - 1, font);
    paint_body(c, w);
  }
  for (int y = 0; y < ph; ++y)
    memcpy(fb_row(T.t + y) + T.l, scratch + size_t(y) * size_t(pw),
           size_t(pw) * 4);
}

void compose_piece(const Rect &p, TrueTypeFont *font) {
  for (int ty = p.t; ty < p.b; ty += kTileH)
    for (int tx = p.l; tx < p.r; tx += kTileW)
      compose_tile(
          {tx, ty, std::min(tx + kTileW, p.r), std::min(ty + kTileH, p.b)},
          font);
}

// ---- input ----------------------------------------------------------------

void mouse_update() {
  const bool left = Kernel::Drivers::Mouse::raw_left_pressed();
  const int mx = cursor_x = Kernel::Drivers::Mouse::get_x();
  const int my = cursor_y = Kernel::Drivers::Mouse::get_y();

  if (left && !old_left) {
    for (size_t n = window_count; n--;) {
      Geo &w = zg[n];
      const size_t idx = w.slot;
      if (mx < w.x - 1 || mx >= w.x + int(w.width) + 1 || my < w.y - 1 ||
          my >= w.y + TITLE_H + int(w.height) + 1)
        continue;
      if (my >= w.y + 5 && my < w.y + TITLE_H - 5 && mx >= w.x + 7 &&
          mx < w.x + 29) {
        const uint64_t tid = windows[idx].thread_id;
        unlink_window(idx);
        Kernel::Scheduler::terminate_thread(tid);
        release_slot(windows[idx]);
      } else {
        focus_window(idx);
        // focus_window() rotated zg: `w` may now alias another window, and
        // the clicked one is guaranteed to be on top.
        const Geo &top = zg[window_count - 1];
        if (my < top.y + TITLE_H) {
          dragging = true;
          drag_x = mx - top.x;
          drag_y = my - top.y;
        }
      }
      break;
    }
  }

  if (left && dragging && window_count) {
    Geo &w = zg[window_count - 1];
    w.x = std::clamp(mx - drag_x, -int(w.width) + 32, int(screen_w) - 32);
    w.y = std::clamp(my - drag_y, 0, int(screen_h) - TITLE_H);
  }

  if (!left)
    dragging = false;
  old_left = left;
}

// ---- planning -------------------------------------------------------------

struct Plan {
  RectList<kMaxPieces> pieces; // pixels to recompose
  RectList<24> moved;          // destination rects of the top window's blit
  int dx = 0, dy = 0;
};

void plan(Plan &p) {
  const Rect screen{0, 0, int(screen_w), int(screen_h)};

  // The top window moves by copying its opaque client pixels row by row.
  // Skipped if damage touches the source (those pixels are stale).
  RectList<24> mv;
  int mdx = 0, mdy = 0;
  if (window_count) {
    const Geo &w = zg[window_count - 1];
    if (w.placed && (w.x != w.sx || w.y != w.sy)) {
      mdx = w.x - w.sx;
      mdy = w.y - w.sy;
      const Rect reach{mdx, mdy, mdx + int(screen_w), mdy + int(screen_h)};
      RectList<24> shape;
      opaque_rects(w, shape, true);
      bool stale = false;
      for (size_t i = 0; i < shape.n; ++i) {
        const Rect m = inter(inter(shape.r[i], screen), reach);
        if (is_empty(m))
          continue;
        const Rect src{m.l - mdx, m.t - mdy, m.r - mdx, m.b - mdy};
        for (size_t k = 0; k < damage_n; ++k)
          stale = stale || overlaps(damage[k], src);
        mv.add(m);
      }
      if (stale || mv.ovf)
        mv.n = 0;
    }
  }

  for (size_t z = 0; z < window_count; ++z) {
    const Geo &w = zg[z];
    if (!w.placed) {
      mark_dirty(ext(w));
    } else if (w.x != w.sx || w.y != w.sy) {
      mark_dirty(ext_at(w.sx, w.sy, w.width, w.height));
      mark_dirty(ext(w));
    }
  }

  for (size_t i = 0; i < damage_n; ++i)
    p.pieces.add(inter(damage[i], screen));
  damage_n = 0;
  if (mv.n) {
    RectList<kMaxPieces> t = p.pieces;
    for (size_t i = 0; i < mv.n; ++i)
      t.subtract(mv.r[i]);
    if (!t.ovf && !p.pieces.ovf) {
      p.pieces = t;
      p.moved = mv;
      p.dx = mdx;
      p.dy = mdy;
    }
  }

  Rect all{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
  for (size_t i = 0; i < p.pieces.n; ++i)
    all = unite(all, p.pieces.r[i]);
  for (size_t z = 0; z < window_count; ++z) {
    Geo &w = zg[z];
    w.placed = true;
    w.sx = w.x;
    w.sy = w.y;
    if (is_empty(all) || !overlaps(body(w), all))
      continue;
    // Expose only when a piece really wipes opaque client pixels.
    RectList<24> shape;
    opaque_rects(w, shape, true);
    bool hit = false;
    for (size_t i = 0; i < shape.n && !hit; ++i)
      for (size_t k = 0; k < p.pieces.n; ++k)
        if (overlaps(shape.r[i], p.pieces.r[k])) {
          hit = true;
          break;
        }
    if (hit)
      st(windows[w.slot].exposed, 1u);
  }
}

void move_pixels(const Plan &p) {
  if (!p.moved.n)
    return;
  int ymin = INT32_MAX, ymax = INT32_MIN;
  for (size_t i = 0; i < p.moved.n; ++i) {
    ymin = std::min(ymin, p.moved.r[i].t);
    ymax = std::max(ymax, p.moved.r[i].b);
  }
  const int rows = ymax - ymin;
  for (int i = 0; i < rows; ++i) {
    const int y = p.dy > 0 ? ymax - 1 - i : ymin + i;
    for (size_t k = 0; k < p.moved.n; ++k) {
      const Rect &m = p.moved.r[k];
      if (y < m.t || y >= m.b)
        continue;
      const int w = m.r - m.l;
      uint32_t *s = fb_row(y - p.dy) + (m.l - p.dx);
      uint32_t *d = fb_row(y) + m.l;
      if (p.dy != 0)
        memcpy(d, s, size_t(w) * 4);
      else if (p.dx < 0)
        for (int q = 0; q < w; ++q)
          d[q] = s[q];
      else
        for (int q = w; q--;)
          d[q] = s[q];
    }
  }
}

// ---- command execution ----------------------------------------------------

template <class F>
inline void for_records(const Cmd::Ring &q, uint32_t t, uint32_t h, F &&f) {
  while (t != h) {
    const uint32_t *rec = q.buf + (t & Cmd::kMask);
    if ((rec[0] & 0xFFu) != Cmd::kPad)
      f(rec);
    t += rec[0] >> 8;
  }
}

// Exact visible shape of window z's client (rounded corners, minus the opaque
// pixels of everything above), split into
//   open   - nobody else's shadow/edge touches it: draw straight to the screen
//   fringe - inside another window's shadow reach: draw, then re-apply the
//            shadows/edges of the windows above, only on pixels that changed.
void build_clips(size_t z, RectList<kMaxClip> &open,
                 RectList<kMaxClip> &fringe) {
  const Geo &w = zg[z];
  const Rect screen{0, 0, int(screen_w), int(screen_h)};
  const Rect cl = client(w);
  {
    RectList<24> shape;
    opaque_rects(w, shape, true);
    for (size_t i = 0; i < shape.n; ++i)
      open.add(inter(shape.r[i], screen));
  }
  for (size_t j = z + 1; j < window_count && open.n; ++j) {
    const Geo &a = zg[j];
    if (!overlaps(body(a), cl))
      continue;
    RectList<24> ar;
    opaque_rects(a, ar, false);
    for (size_t i = 0; i < ar.n; ++i)
      if (overlaps(ar.r[i], cl))
        open.subtract(ar.r[i]);
  }
  for (size_t j = z + 1; j < window_count && open.n; ++j) {
    const Rect e = ext(zg[j]);
    if (!overlaps(e, cl))
      continue;
    for (size_t i = 0; i < open.n; ++i)
      if (overlaps(open.r[i], e))
        fringe.add(inter(open.r[i], e));
    open.subtract(e);
  }
}

void run_fringe_tile(size_t z, const Geo &w, const Rect &T, const Cmd::Ring &q,
                     uint32_t t0, uint32_t h) {
  const int pw = T.r - T.l, ph = T.b - T.t;
  for (int y = 0; y < ph; ++y)
    memcpy(scratch + size_t(y) * size_t(pw), fb_row(T.t + y) + T.l,
           size_t(pw) * 4);

  const int cl_l = w.x, cl_t = w.y + TITLE_H;
  const Canvas cs{scratch + ptrdiff_t(cl_t - T.t) * pw + (cl_l - T.l),
                  pw,
                  T.l - cl_l,
                  T.t - cl_t,
                  T.r - cl_l,
                  T.b - cl_t};
  for_records(q, t0, h, [&](const uint32_t *rec) { Raster::execute(cs, rec); });

  size_t n_changed = 0;
  for (int y = 0; y < ph; ++y) {
    const uint32_t *s = scratch + size_t(y) * size_t(pw);
    const uint32_t *f = fb_row(T.t + y) + T.l;
    uint8_t *m = changed + size_t(y) * size_t(pw);
    for (int x = 0; x < pw; ++x) {
      m[x] = s[x] != f[x];
      n_changed += m[x];
    }
  }
  if (!n_changed)
    return;

  const Canvas ca{scratch - ptrdiff_t(T.t) * pw - T.l, pw, T.l, T.t, T.r, T.b};
  for (size_t j = z + 1; j < window_count; ++j) {
    const Geo &a = zg[j];
    if (!overlaps(ext(a), T))
      continue;
    paint_shadow(ca, a);
    paint_edges(ca, a);
  }

  for (int y = 0; y < ph; ++y) {
    const uint32_t *s = scratch + size_t(y) * size_t(pw);
    const uint8_t *m = changed + size_t(y) * size_t(pw);
    uint32_t *d = fb_row(T.t + y) + T.l;
    for (int x = 0; x < pw;) {
      if (!m[x]) {
        ++x;
        continue;
      }
      int e = x + 1;
      while (e < pw && m[e])
        ++e;
      memcpy(d + x, s + x, size_t(e - x) * 4);
      x = e;
    }
  }
}

bool snapshot_rings() {
  bool any = false;
  for (size_t z = 0; z < window_count; ++z) {
    const size_t idx = zg[z].slot;
    g_heads[idx] = ld(windows[idx].ring.head);
    any = any || windows[idx].ring.tail != g_heads[idx];
  }
  return any;
}

void drain_all() {
  for (size_t z = 0; z < window_count; ++z) {
    const Geo &w = zg[z];
    const size_t idx = w.slot;
    Cmd::Ring &q = windows[idx].ring;
    const uint32_t t = q.tail, h = g_heads[idx];
    if (t == h)
      continue;

    RectList<kMaxClip> open, fringe;
    build_clips(z, open, fringe);

    if (open.n) {
      Canvas cv[kMaxClip];
      const Rect cl = client(w);
      uint32_t *origin = fb + ptrdiff_t(cl.t) * fb_stride + cl.l;
      for (size_t i = 0; i < open.n; ++i)
        cv[i] = {origin,
                 fb_stride,
                 open.r[i].l - cl.l,
                 open.r[i].t - cl.t,
                 open.r[i].r - cl.l,
                 open.r[i].b - cl.t};
      const size_t n = open.n;
      for_records(q, t, h, [&](const uint32_t *rec) {
        for (size_t i = 0; i < n; ++i)
          Raster::execute(cv[i], rec);
      });
    }
    for (size_t i = 0; i < fringe.n; ++i) {
      const Rect &p = fringe.r[i];
      for (int ty = p.t; ty < p.b; ty += kTileH)
        for (int tx = p.l; tx < p.r; tx += kTileW)
          run_fringe_tile(
              z, w,
              {tx, ty, std::min(tx + kTileW, p.r), std::min(ty + kTileH, p.b)},
              q, t, h);
    }
    st(q.tail, h);
  }
}

// Would this frame write a pixel the cursor is sitting on?
bool frame_touches(const Rect &cb, const Plan &p) {
  for (size_t i = 0; i < p.pieces.n; ++i)
    if (overlaps(cb, p.pieces.r[i]))
      return true;
  for (size_t i = 0; i < p.moved.n; ++i) {
    const Rect &m = p.moved.r[i];
    const Rect src{m.l - p.dx, m.t - p.dy, m.r - p.dx, m.b - p.dy};
    if (overlaps(cb, m) || overlaps(cb, src))
      return true;
  }
  for (size_t z = 0; z < window_count; ++z) {
    const size_t idx = zg[z].slot;
    if (windows[idx].ring.tail != g_heads[idx] && overlaps(cb, client(zg[z])))
      return true;
  }
  return false;
}

bool tick() {
  sync_table();
  mouse_update();
  publish();

  Plan p;
  plan(p);
  const bool pending = snapshot_rings();
  const bool moved = !cursor_on || cursor_x != cur_x || cursor_y != cur_y;
  if (!p.pieces.n && !p.moved.n && !moved && !pending)
    return false;

  // The cursor is only lifted when it moved or this frame writes under it.
  if (cursor_on && (moved || frame_touches(cursor_box(), p)))
    cursor_hide();

  move_pixels(p);
  TrueTypeFont *font = Raster::default_font();
  for (size_t i = 0; i < p.pieces.n; ++i)
    compose_piece(p.pieces.r[i], font);
  drain_all();
  if (!cursor_on)
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
  init_corners();
  init_shadows();
  nspans = ncells = 0;
  for (int dy = -CURSOR_R; dy <= CURSOR_R; ++dy) {
    int dx = -CURSOR_R;
    while (dx <= CURSOR_R) {
      int sh = cursor_shape(dx, dy);
      if (!sh) {
        ++dx;
        continue;
      }
      const uint32_t color = sh == 1 ? BLACK : TEXT;
      const int x0 = dx;
      do {
        ++dx;
        if (dx > CURSOR_R)
          break;
        sh = cursor_shape(dx, dy);
      } while (sh && (sh == 1 ? BLACK : TEXT) == color);
      const int x1 = dx - 1;
      cursor_spans[nspans++] = {int8_t(dy), int8_t(x0), int8_t(x1), color,
                                ncells};
      ncells += size_t(x1 - x0 + 1);
    }
  }
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
    w.init_x = x;
    w.init_y = y;
    w.req_size = pack(width, height);
    w.raise_req = 0;
    w.exposed = 1;
    w.pos_x = x;
    w.pos_y = y + TITLE_H;
    w.known = false;
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
