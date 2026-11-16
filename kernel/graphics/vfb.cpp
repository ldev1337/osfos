// vfb.cpp - VirtualFramebuffer: every primitive draws straight into the
// hardware framebuffer (or a caller-owned buffer), clipped by the window
// manager. Nothing is allocated.
//
//  * window mode (open_window()):  draws into the thread's window client area
//  * screen mode (init(x,y,w,h)):  draws at (x,y) on the raw screen
//  * memory mode (attach(buf,..)): draws into a caller-owned buffer
//
// Anti-aliasing is 2x2 supersampling on the lattice (4x+1|3, 4y+1|3), i.e. a
// pixel owns the odd sample columns 4x+1 and 4x+3. Discs, rings, triangles and
// sectors are scan-converted from per-sample-row intervals: only edge pixels
// are evaluated, interiors are plain stores and never read the destination.
#include <algorithm>
#include <arch/x86_64/string.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/lwm_draw.h>
#include <kernel/graphics/open_font.h>
#include <kernel/graphics/simplefonts.h>
#include <kernel/graphics/truetype.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/math.h>
#include <kernel/scheduler.h>

// temp abs fix
namespace std {
static inline int abs(int x) { return x < 0 ? -x : x; }
} // namespace std

namespace Kernel::Graphics {
namespace {

struct Box {
  int32_t l, t, r, b;
};

// Intersects [l,r) x [t,b) with the canvas clip.
inline bool box(const Canvas &c, int64_t l, int64_t t, int64_t r, int64_t b,
                Box &o) {
  o.l = int32_t(std::max<int64_t>(l, c.l));
  o.t = int32_t(std::max<int64_t>(t, c.t));
  o.r = int32_t(std::min<int64_t>(r, c.r));
  o.b = int32_t(std::min<int64_t>(b, c.b));
  return o.l < o.r && o.t < o.b;
}

inline uint32_t *row_ptr(const Canvas &c, int32_t y) {
  return c.origin + ptrdiff_t(y) * c.stride;
}

inline void fill32(uint32_t *p, uint32_t v, size_t n) {
  if (n < 16) {
    while (n--)
      *p++ = v;
    return;
  }
  asm volatile("rep stosl" : "+D"(p), "+c"(n) : "a"(v) : "memory");
}

// Exact per-channel (fg*a + bg*(255-a)) / 255 with rounding; R and B share one
// multiply. Valid for any a, callers handle the 0 / 255 shortcuts.
inline uint32_t mix(uint32_t fg, uint32_t bg, uint32_t a) {
  const uint32_t ia = 255 - a;
  uint32_t rb = (fg & 0x00FF00FFu) * a + (bg & 0x00FF00FFu) * ia + 0x00800080u;
  rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
  uint32_t g = ((fg >> 8) & 255) * a + ((bg >> 8) & 255) * ia + 128;
  g = (g + (g >> 8)) >> 8;
  return 0xFF000000u | rb | (g << 8);
}

// samples in 0..4 scaled by alpha; fully covered pixels are written blind.
inline void put_cov(const Canvas &c, int32_t x, int32_t y, uint32_t color,
                    uint32_t samples, uint32_t alpha = 255) {
  const uint32_t cov = (samples * alpha + 2) / 4;
  if (!cov)
    return;
  uint32_t &p = row_ptr(c, y)[x];
  p = cov >= 255 ? color : mix(color, p, cov);
}

inline void span_fill(uint32_t *p, int64_t n, uint32_t color, uint32_t alpha) {
  if (alpha >= 255)
    return fill32(p, color, size_t(n));
  for (int64_t i = 0; i < n; ++i)
    p[i] = mix(color, p[i], alpha);
}

// ---- 8x8 bitmap text --------------------------------------------------------

void glyph(const Canvas &c, int32_t x, int32_t y, char ch, uint32_t fg,
           uint32_t bg, bool opaque) {
  unsigned u = static_cast<unsigned char>(ch);
  if (u < 32 || u > 127)
    u = '?';
  const uint8_t *bits = Font::Basic8x8[u - 32];
  Box b;
  if (!box(c, x, y, int64_t(x) + 8, int64_t(y) + 8, b))
    return;
  for (int32_t cy = b.t; cy < b.b; ++cy) {
    uint32_t *row = row_ptr(c, cy);
    const unsigned line = bits[cy - y];
    if (opaque)
      for (int32_t cx = b.l; cx < b.r; ++cx)
        row[cx] = (line >> (cx - x)) & 1 ? fg : bg;
    else
      for (int32_t cx = b.l; cx < b.r; ++cx)
        if ((line >> (cx - x)) & 1)
          row[cx] = fg;
  }
}

// Nearest-neighbour scaled 8x8 text; used when no TrueType font is present.
int text8(const Canvas &c, int32_t x, int32_t y, float size, const char *text,
          uint32_t color) {
  const int gs = std::max(1, int(size + 0.5f));
  int cursor = x;
  for (const char *p = text; *p; ++p, cursor += gs) {
    unsigned ch = static_cast<unsigned char>(*p);
    if (ch < 32 || ch > 127)
      ch = '?';
    const uint8_t *bits = Font::Basic8x8[ch - 32];
    Box b;
    if (!box(c, cursor, y, int64_t(cursor) + gs, int64_t(y) + gs, b))
      continue;
    for (int32_t ty = b.t; ty < b.b; ++ty) {
      uint32_t *row = row_ptr(c, ty);
      const unsigned line = bits[(ty - y) * 8 / gs];
      for (int32_t tx = b.l; tx < b.r; ++tx)
        if ((line >> ((tx - cursor) * 8 / gs)) & 1)
          row[tx] = color;
    }
  }
  return cursor;
}

// ---- sample-interval scan conversion ---------------------------------------

// Samples (all integers; only odd ones exist) in [lo, hi]; empty if lo > hi.
struct Iv {
  int64_t lo = 1, hi = 0;
  bool empty() const { return lo > hi; }
};
struct Iv2 { // two disjoint intervals, a left of b; b only used when a is
  Iv a, b;
};

inline int64_t isqrt(int64_t v) { // floor(sqrt(v)), v >= 0
  if (v < 2)
    return v;
  int64_t r = 0, bit = int64_t(1) << (62 - (__builtin_clzll(v) & ~1));
  for (; bit; bit >>= 2) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
  }
  return r;
}

inline int64_t floordiv(int64_t a, int64_t b) { // b > 0
  return a >= 0 ? a / b : -((-a + b - 1) / b);
}
inline int64_t ceildiv(int64_t a, int64_t b) { return -floordiv(-a, b); }

// Restricts v to { u : k*u + c >= 0 }.
inline void halfplane(Iv &v, int64_t k, int64_t c) {
  if (k > 0)
    v.lo = std::max(v.lo, ceildiv(-c, k));
  else if (k < 0)
    v.hi = std::min(v.hi, floordiv(c, -k));
  else if (c < 0)
    v = Iv{1, 0};
}

inline uint32_t in(int64_t s, const Iv &v) { return v.lo <= s && s <= v.hi; }

inline uint32_t samples_at(int32_t x, const Iv2 (&s)[2]) {
  const int64_t p = int64_t(x) * 4 + 1;
  return in(p, s[0].a) + in(p + 2, s[0].a) + in(p, s[0].b) + in(p + 2, s[0].b) +
         in(p, s[1].a) + in(p + 2, s[1].a) + in(p, s[1].b) + in(p + 2, s[1].b);
}

// Paints pixel row y, x in [xl, xr), from the covered sample intervals of its
// two sample rows. Solid runs are stored blind, holes are skipped.
void cover_row(const Canvas &c, int32_t y, int32_t xl, int32_t xr,
               const Iv2 (&s)[2], uint32_t color, uint32_t alpha) {
  int64_t lo = INT64_MAX, hi = INT64_MIN;
  for (const Iv *v : {&s[0].a, &s[0].b, &s[1].a, &s[1].b})
    if (!v->empty()) {
      lo = std::min(lo, v->lo);
      hi = std::max(hi, v->hi);
    }
  if (lo > hi)
    return;
  const int64_t a = std::max<int64_t>((lo | 1) >> 2, xl);
  const int64_t z = std::min<int64_t>(((hi - 1) | 1) >> 2, int64_t(xr) - 1);
  if (a > z)
    return;

  int64_t fa = 1, fz = 0, ha = 1, hz = 0; // solid run / empty hole (pixels)
  const bool sp0 = !s[0].b.empty(), sp1 = !s[1].b.empty();
  if (!sp0 && !sp1) {
    if (!s[0].a.empty() && !s[1].a.empty()) {
      fa = (std::max(s[0].a.lo, s[1].a.lo) + 2) >> 2;
      fz = (std::min(s[0].a.hi, s[1].a.hi) - 3) >> 2;
    }
  } else if (sp0 && sp1) {
    ha = (std::max(s[0].a.hi, s[1].a.hi) + 3) >> 2;
    hz = (std::min(s[0].b.lo, s[1].b.lo) - 4) >> 2;
  }

  uint32_t *row = row_ptr(c, y);
  for (int64_t x = a; x <= z; ++x) {
    if (x >= fa && x <= fz) {
      const int64_t e = std::min(fz, z);
      span_fill(row + x, e - x + 1, color, alpha);
      x = e;
    } else if (x >= ha && x <= hz) {
      x = std::min(hz, z);
    } else {
      put_cov(c, int32_t(x), y, color, samples_at(int32_t(x), s), alpha);
    }
  }
}

// Disc: centre (cx,cy) and radius R in sample units, restricted to box b.
void disc(const Canvas &c, const Box &b, int64_t cx, int64_t cy, int64_t R,
          uint32_t color, uint32_t alpha) {
  const int64_t R2 = R * R;
  for (int32_t y = b.t; y < b.b; ++y) {
    Iv2 s[2];
    for (int k = 0; k < 2; ++k) {
      const int64_t dy = int64_t(y) * 4 + 1 + 2 * k - cy;
      if (const int64_t rem = R2 - dy * dy; rem >= 0) {
        const int64_t m = isqrt(rem);
        s[k].a = Iv{cx - m, cx + m};
      }
    }
    cover_row(c, y, b.l, b.r, s, color, alpha);
  }
}

// Ring Ri <= d <= Ro (sample units) restricted to box b.
void ring(const Canvas &c, const Box &b, int64_t cx, int64_t cy, int64_t Ro,
          int64_t Ri, uint32_t color, uint32_t alpha) {
  for (int32_t y = b.t; y < b.b; ++y) {
    Iv2 s[2];
    for (int k = 0; k < 2; ++k) {
      const int64_t dy = int64_t(y) * 4 + 1 + 2 * k - cy;
      const int64_t ro = Ro * Ro - dy * dy;
      if (ro < 0)
        continue;
      const int64_t mo = isqrt(ro);
      if (const int64_t ri = Ri * Ri - dy * dy - 1; ri < 0) {
        s[k].a = Iv{cx - mo, cx + mo};
      } else {
        const int64_t mi = isqrt(ri);
        s[k].a = Iv{cx - mo, cx - mi - 1};
        s[k].b = Iv{cx + mi + 1, cx + mo};
      }
    }
    cover_row(c, y, b.l, b.r, s, color, alpha);
  }
}

void circle(const Canvas &c, int32_t xc, int32_t yc, int32_t r, uint32_t color,
            uint32_t alpha, bool outline) {
  if (r < 0)
    return;
  if (r == 0) {
    if (xc >= c.l && xc < c.r && yc >= c.t && yc < c.b)
      put_cov(c, xc, yc, color, 4, alpha);
    return;
  }
  Box b;
  if (!box(c, int64_t(xc) - r - 1, int64_t(yc) - r - 1, int64_t(xc) + r + 2,
           int64_t(yc) + r + 2, b))
    return;
  const int64_t cx = int64_t(xc) * 4 + 2, cy = int64_t(yc) * 4 + 2;
  if (outline)
    ring(c, b, cx, cy, int64_t(r) * 4 + 2, std::max<int64_t>(r * 4 - 2, 0),
         color, alpha);
  else
    disc(c, b, cx, cy, int64_t(r) * 4, color, alpha);
}

void triangle(const Canvas &c, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
              int32_t x2, int32_t y2, uint32_t color) {
  const int64_t px[3] = {int64_t(x0) * 4 + 2, int64_t(x1) * 4 + 2,
                         int64_t(x2) * 4 + 2};
  const int64_t py[3] = {int64_t(y0) * 4 + 2, int64_t(y1) * 4 + 2,
                         int64_t(y2) * 4 + 2};
  const int64_t area =
      (px[1] - px[0]) * (py[2] - py[0]) - (py[1] - py[0]) * (px[2] - px[0]);
  if (!area)
    return;
  Box b;
  if (!box(c, std::min({x0, x1, x2}) - 1, std::min({y0, y1, y2}) - 1,
           int64_t(std::max({x0, x1, x2})) + 2,
           int64_t(std::max({y0, y1, y2})) + 2, b))
    return;
  const int64_t sg = area > 0 ? 1 : -1;
  for (int32_t y = b.t; y < b.b; ++y) {
    Iv2 s[2];
    for (int k = 0; k < 2; ++k) {
      const int64_t sy = int64_t(y) * 4 + 1 + 2 * k;
      Iv v{INT64_MIN / 2, INT64_MAX / 2};
      for (int e = 0; e < 3 && !v.empty(); ++e) {
        const int n = (e + 1) % 3;
        const int64_t ex = px[n] - px[e], ey = py[n] - py[e];
        halfplane(v, -ey * sg, (ex * (sy - py[e]) + ey * px[e]) * sg);
      }
      s[k].a = v;
    }
    cover_row(c, y, b.l, b.r, s, color, 255);
  }
}

// Pie slice, angles in degrees (clockwise on screen). One pass: no seams.
void sector(const Canvas &c, int32_t xc, int32_t yc, int32_t r, int32_t start,
            int32_t end, uint32_t color) {
  if (r < 0 || end <= start)
    return;
  Box b;
  if (!box(c, int64_t(xc) - r - 1, int64_t(yc) - r - 1, int64_t(xc) + r + 2,
           int64_t(yc) + r + 2, b))
    return;
  using Kernel::Lib::Math::fixed_cos;
  using Kernel::Lib::Math::fixed_sin;
  const int64_t cx = int64_t(xc) * 4 + 2, cy = int64_t(yc) * 4 + 2;
  const int64_t R2 = int64_t(r) * 4 * (int64_t(r) * 4);
  const int64_t d0x = fixed_cos(start), d0y = fixed_sin(start);
  const int64_t d1x = fixed_cos(end), d1y = fixed_sin(end);
  const bool whole = end - start >= 360, big = end - start > 180;
  for (int32_t y = b.t; y < b.b; ++y) {
    Iv2 s[2];
    for (int k = 0; k < 2; ++k) {
      const int64_t dy = int64_t(y) * 4 + 1 + 2 * k - cy;
      const int64_t rem = R2 - dy * dy;
      if (rem < 0)
        continue;
      const int64_t m = isqrt(rem);
      const Iv d{cx - m, cx + m};
      if (whole) {
        s[k].a = d;
      } else if (!big) { // between the two radii
        Iv v = d;
        halfplane(v, -d0y, d0x * dy + d0y * cx);
        halfplane(v, d1y, -d1y * cx - dy * d1x);
        s[k].a = v;
      } else { // disc minus the (convex) complementary wedge
        Iv w = d;
        halfplane(w, -d1y, d1x * dy + d1y * cx);
        halfplane(w, d0y, -d0y * cx - dy * d0x);
        if (w.empty()) {
          s[k].a = d;
        } else {
          Iv l{d.lo, w.lo - 1}, rr{w.hi + 1, d.hi};
          if (l.empty())
            std::swap(l, rr);
          s[k].a = l;
          s[k].b = rr;
        }
      }
    }
    cover_row(c, y, b.l, b.r, s, color, 255);
  }
}

// ---- lines
// -------------------------------------------------------------------

struct Seg {
  int64_t ax, ay, dx, dy, len2;
};

// Sample within 0.5px (2 sample units) of the segment (stadium shape).
inline bool near_seg(const Seg &s, int64_t px, int64_t py) {
  const int64_t ox = px - s.ax, oy = py - s.ay;
  const int64_t proj = ox * s.dx + oy * s.dy;
  if (proj <= 0)
    return ox * ox + oy * oy <= 4;
  if (proj >= s.len2) {
    const int64_t qx = ox - s.dx, qy = oy - s.dy;
    return qx * qx + qy * qy <= 4;
  }
  const int64_t cr = ox * s.dy - oy * s.dx;
  return cr * cr <= 4 * s.len2;
}

void line(const Canvas &c, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
          uint32_t color) {
  if (x0 == x1 || y0 == y1) { // axis-aligned 1px line == exact pixel run
    Raster::fill_rect(c, std::min(x0, x1), std::min(y0, y1),
                      std::abs(x1 - x0) + 1, std::abs(y1 - y0) + 1, color);
    return;
  }
  const Seg s{int64_t(x0) * 4 + 2, int64_t(y0) * 4 + 2, int64_t(x1 - x0) * 4,
              int64_t(y1 - y0) * 4,
              (int64_t(x1 - x0) * 4) * (int64_t(x1 - x0) * 4) +
                  (int64_t(y1 - y0) * 4) * (int64_t(y1 - y0) * 4)};
  auto plot = [&](int32_t x, int32_t y) {
    const int64_t px = int64_t(x) * 4, py = int64_t(y) * 4;
    put_cov(c, x, y, color,
            near_seg(s, px + 1, py + 1) + near_seg(s, px + 3, py + 1) +
                near_seg(s, px + 1, py + 3) + near_seg(s, px + 3, py + 3));
  };
  // Walk the major axis; only +-3 pixels around the centre line can be covered.
  if (std::abs(x1 - x0) >= std::abs(y1 - y0)) {
    if (x0 > x1) {
      std::swap(x0, x1);
      std::swap(y0, y1);
    }
    const int64_t dx = x1 - x0, dy = y1 - y0;
    for (int32_t x = std::max(x0 - 1, c.l), xe = std::min(x1 + 2, c.r); x < xe;
         ++x) {
      const int32_t xc = std::clamp(x, x0, x1);
      const int32_t yc = y0 + int32_t((int64_t(xc - x0) * dy + dx / 2) / dx);
      for (int32_t y = std::max(yc - 3, c.t), ye = std::min(yc + 4, c.b);
           y < ye; ++y)
        plot(x, y);
    }
  } else {
    if (y0 > y1) {
      std::swap(x0, x1);
      std::swap(y0, y1);
    }
    const int64_t dx = x1 - x0, dy = y1 - y0;
    for (int32_t y = std::max(y0 - 1, c.t), ye = std::min(y1 + 2, c.b); y < ye;
         ++y) {
      const int32_t yc = std::clamp(y, y0, y1);
      const int32_t xc = x0 + int32_t((int64_t(yc - y0) * dx + dy / 2) / dy);
      for (int32_t x = std::max(xc - 3, c.l), xe = std::min(xc + 4, c.r);
           x < xe; ++x)
        plot(x, y);
    }
  }
}

template <typename Draw>
void walk_arc(int32_t xc, int32_t yc, int32_t r, int32_t start, int32_t end,
              Draw draw) {
  int32_t px = 0, py = 0;
  for (int32_t deg = start; deg <= end; ++deg) {
    const int32_t x = xc + r * Kernel::Lib::Math::fixed_cos(deg) / 1024;
    const int32_t y = yc + r * Kernel::Lib::Math::fixed_sin(deg) / 1024;
    if (deg != start)
      draw(px, py, x, y);
    px = x;
    py = y;
  }
}

void rect_outline(const Canvas &c, int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t color) {
  if (w <= 0 || h <= 0)
    return;
  Raster::fill_rect(c, x, y, w, 1, color);
  Raster::fill_rect(c, x, y + h - 1, w, 1, color);
  Raster::fill_rect(c, x, y, 1, h, color);
  Raster::fill_rect(c, x + w - 1, y, 1, h, color);
}

void rounded_outline(const Canvas &c, int32_t rx, int32_t ry, int32_t w,
                     int32_t h, int32_t radius, uint32_t color) {
  radius = std::min(radius, std::min(w, h) / 2);
  if (radius <= 0)
    return rect_outline(c, rx, ry, w, h, color);
  const int32_t r = rx + w - 1, b = ry + h - 1;
  Raster::fill_rect(c, rx + radius, ry, w - 2 * radius, 1, color);
  Raster::fill_rect(c, rx + radius, b, w - 2 * radius, 1, color);
  Raster::fill_rect(c, rx, ry + radius, 1, h - 2 * radius, color);
  Raster::fill_rect(c, r, ry + radius, 1, h - 2 * radius, color);

  const int64_t Ro = int64_t(radius) * 4 + 2;
  const int64_t Ri = std::max<int64_t>(int64_t(radius) * 4 - 2, 0);
  // Corner arc = ring restricted to the quadrant box of its centre pixel.
  auto corner = [&](int32_t cx, int32_t cy, bool left, bool top) {
    Box q;
    if (box(c, left ? cx - radius - 1 : cx, top ? cy - radius - 1 : cy,
            left ? cx + 1 : cx + radius + 2, top ? cy + 1 : cy + radius + 2, q))
      ring(c, q, int64_t(cx) * 4 + 2, int64_t(cy) * 4 + 2, Ro, Ri, color, 255);
  };
  corner(rx + radius, ry + radius, true, true);
  corner(r - radius, ry + radius, false, true);
  corner(rx + radius, b - radius, true, false);
  corner(r - radius, b - radius, false, false);
}

template <typename Pixel>
void blit_pixels(const Canvas &c, int64_t bx, int64_t by, uint32_t bw,
                 uint32_t bh, const uint32_t *src, uint32_t src_pitch,
                 Pixel pixel) {
  Box b;
  if (!box(c, bx, by, bx + bw, by + bh, b))
    return;
  const uint8_t *srow = reinterpret_cast<const uint8_t *>(src) +
                        size_t(b.t - by) * src_pitch +
                        size_t(b.l - bx) * sizeof(uint32_t);
  const int32_t n = b.r - b.l;
  for (int32_t y = b.t; y < b.b; ++y, srow += src_pitch) {
    const auto *s = reinterpret_cast<const uint32_t *>(srow);
    uint32_t *d = row_ptr(c, y) + b.l;
    for (int32_t x = 0; x < n; ++x)
      d[x] = pixel(s[x], d[x]);
  }
}

constexpr float kGlyphSize = 16.0f;
constexpr int kGlyphBgHeight = 18;

bool glyph_ttf(const Canvas &c, int32_t gx, int32_t gy, char ch, uint32_t fg,
               uint32_t bg, bool fill_bg) {
  TrueTypeFont *font = Raster::default_font();
  if (!font)
    return false;
  const char text[2] = {ch, '\0'};
  if (fill_bg)
    Raster::fill_rect(c, gx, gy, font->get_text_width(kGlyphSize, text),
                      kGlyphBgHeight, bg);
  Raster::text(c, *font, gx, gy, kGlyphSize, text, fg);
  return true;
}

// ---- how a VirtualFramebuffer reaches pixels --------------------------------

struct View {
  uint64_t tid;
  int32_t x, y;
  uint32_t w, h;
  uint32_t *buf; // memory mode only
  uint32_t pitch;
  bool windowed;
};

// Runs fn(const Canvas&) once per visible clip rectangle. (bx,by,bw,bh) is the
// local bounding box that will be touched (used to hide the cursor).
template <class F>
void paint(const View &v, int32_t bx, int32_t by, int32_t bw, int32_t bh,
           F &&fn) {
  using namespace WindowManager;
  if (v.buf) {
    fn(Canvas{v.buf, ptrdiff_t(v.pitch / 4), 0, 0, int32_t(v.w), int32_t(v.h)});
    return;
  }
  Surface s;
  if (begin_draw(v.tid, v.windowed, v.x, v.y, bx, by, bw, bh, s) !=
      DrawStatus::Ok)
    return;
  for (uint32_t i = 0; i < s.n; ++i) {
    const Canvas c{s.origin,
                   s.stride,
                   std::max<int32_t>(s.clip[i].l - s.ox, 0),
                   std::max<int32_t>(s.clip[i].t - s.oy, 0),
                   std::min<int32_t>(s.clip[i].r - s.ox, int32_t(v.w)),
                   std::min<int32_t>(s.clip[i].b - s.oy, int32_t(v.h))};
    if (c.l < c.r && c.t < c.b)
      fn(c);
  }
  end_draw();
}

} // namespace

// Raster: exported primitives (also used by the compositor)
namespace Raster {

TrueTypeFont *default_font() {
  static TrueTypeFont font;
  static int state = 0; // 0 = not tried, 1 = loaded, 2 = unavailable
  if (!state)
    state = (OpenFont::load_embedded_named(font, "GeistMono.ttf") ||
             OpenFont::load_preferred(font, "GeistMono.ttf", "Geist.ttf")) &&
                    font.is_valid()
                ? 1
                : 2;
  return state == 1 ? &font : nullptr;
}

uint32_t blend(uint32_t fg, uint32_t bg, uint32_t alpha) {
  return alpha >= 255 ? fg : alpha ? mix(fg, bg, alpha) : bg;
}

void fill_rect(const Canvas &c, int32_t x, int32_t y, int32_t w, int32_t h,
               uint32_t color) {
  Box b;
  if (!box(c, x, y, int64_t(x) + w, int64_t(y) + h, b))
    return;
  const size_t n = size_t(b.r - b.l);
  uint32_t *row = row_ptr(c, b.t) + b.l;
  if (ptrdiff_t(n) == c.stride) // rows are contiguous
    return fill32(row, color, n * size_t(b.b - b.t));
  for (int32_t y2 = b.t; y2 < b.b; ++y2, row += c.stride)
    fill32(row, color, n);
}

void fill_circle(const Canvas &c, int32_t xc, int32_t yc, int32_t r,
                 uint32_t color) {
  circle(c, xc, yc, r, color, 255, false);
}

void fill_rounded_rect(const Canvas &c, int32_t rx, int32_t ry, int32_t w,
                       int32_t h, int32_t radius, uint32_t color) {
  radius = std::min(radius, std::min(w, h) / 2);
  if (radius <= 0)
    return fill_rect(c, rx, ry, w, h, color);
  fill_rect(c, rx + radius, ry, w - 2 * radius, h, color);
  fill_rect(c, rx, ry + radius, radius, h - 2 * radius, color);
  fill_rect(c, rx + w - radius, ry + radius, radius, h - 2 * radius, color);
  const int64_t R = int64_t(radius) * 4;
  auto corner = [&](int32_t left, int32_t top, int32_t cx, int32_t cy) {
    Box b;
    if (box(c, left, top, int64_t(left) + radius, int64_t(top) + radius, b))
      disc(c, b, int64_t(cx) * 4, int64_t(cy) * 4, R, color, 255);
  };
  const int32_t fx = rx + w - radius, fy = ry + h - radius;
  corner(rx, ry, rx + radius, ry + radius);
  corner(fx, ry, fx, ry + radius);
  corner(rx, fy, rx + radius, fy);
  corner(fx, fy, fx, fy);
}

int text(const Canvas &c, TrueTypeFont &font, int32_t x, int32_t y, float size,
         const char *str, uint32_t color) {
  uint32_t *sub = row_ptr(c, c.t) + c.l; // clip origin as a sub-buffer
  return font.render_text(sub, uint32_t(c.r - c.l), uint32_t(c.b - c.t),
                          uint32_t(c.stride * 4), x - c.l, y - c.t, size, str,
                          color) +
         c.l;
}

void glyph8(const Canvas &c, int32_t x, int32_t y, char ch, uint32_t color) {
  glyph(c, x, y, ch, color, 0, false);
}

} // namespace Raster

// ============================================================================
// VirtualFramebuffer
// ============================================================================
#define VFB_VIEW                                                               \
  View {                                                                       \
    Kernel::Scheduler::current_thread_id(), x_, y_, width_, height_, buffer_,  \
        static_cast<uint32_t>(pitch_), owns_buffer_                            \
  }

VirtualFramebuffer::VirtualFramebuffer(int32_t x, int32_t y, uint32_t width,
                                       uint32_t height) {
  init(x, y, width, height);
}

VirtualFramebuffer::~VirtualFramebuffer() { destroy(); }

VirtualFramebuffer::VirtualFramebuffer(VirtualFramebuffer &&other) noexcept {
  *this = static_cast<VirtualFramebuffer &&>(other);
}

VirtualFramebuffer &
VirtualFramebuffer::operator=(VirtualFramebuffer &&other) noexcept {
  if (this != &other) {
    x_ = other.x_;
    y_ = other.y_;
    width_ = other.width_;
    height_ = other.height_;
    buffer_ = other.buffer_;
    pitch_ = other.pitch_;
    initialized_ = other.initialized_;
    owns_buffer_ = other.owns_buffer_;
    other.destroy();
  }
  return *this;
}

// Screen mode. (owns_buffer_ doubles as "bound to this thread's window".)
bool VirtualFramebuffer::init(int32_t x, int32_t y, uint32_t width,
                              uint32_t height) {
  return attach(nullptr, x, y, width, height);
}

// Memory mode: draw into a caller-owned buffer (never freed here).
bool VirtualFramebuffer::attach(uint32_t *buffer, int32_t x, int32_t y,
                                uint32_t width, uint32_t height) {
  destroy();
  if (width == 0 || height == 0)
    return false;
  x_ = x;
  y_ = y;
  width_ = width;
  height_ = height;
  pitch_ = width * sizeof(uint32_t);
  buffer_ = buffer;
  initialized_ = true;
  return true;
}

// Window mode: reserves a window; pixels go straight to the screen inside it.
bool VirtualFramebuffer::open_window(uint32_t requested_width,
                                     uint32_t requested_height, uint32_t,
                                     uint32_t) {
  uint32_t width = 0, height = 0;
  if (!Kernel::Graphics::WindowManager::request_window_size(
          Kernel::Scheduler::current_thread_id(), requested_width,
          requested_height, &width, &height) ||
      !init(0, 0, width, height)) {
    destroy();
    return false;
  }
  owns_buffer_ = true;
  return true;
}

bool VirtualFramebuffer::has_focus() const {
  return Kernel::Graphics::WindowManager::has_keyboard_focus(
      Kernel::Scheduler::current_thread_id());
}

void VirtualFramebuffer::destroy() {
  buffer_ = nullptr;
  owns_buffer_ = false;
  initialized_ = false;
  width_ = height_ = 0;
}

// Direct modes already put every pixel on screen. Memory mode publishes.
void VirtualFramebuffer::flush() const {
  if (!buffer_)
    return;
  if (!Kernel::Graphics::WindowManager::present(
          Kernel::Scheduler::current_thread_id(), buffer_, width_, height_,
          pitch_))
    Kernel::Drivers::Framebuffer::blit(buffer_, uint32_t(x_), uint32_t(y_),
                                       width_, height_, pitch_);
}

void VirtualFramebuffer::put_pixel(int32_t px, int32_t py, uint32_t color) {
  if (uint32_t(px) >= width_ || uint32_t(py) >= height_) [[unlikely]]
    return;
  paint(VFB_VIEW, px, py, 1, 1, [&](const Canvas &c) {
    if (px >= c.l && px < c.r && py >= c.t && py < c.b)
      row_ptr(c, py)[px] = color;
  });
}

void VirtualFramebuffer::fill(uint32_t color) {
  fill_rect(0, 0, int32_t(width_), int32_t(height_), color);
}

void VirtualFramebuffer::fill_rect(int32_t rx, int32_t ry, int32_t rw,
                                   int32_t rh, uint32_t color) {
  paint(VFB_VIEW, rx, ry, rw, rh,
        [&](const Canvas &c) { Raster::fill_rect(c, rx, ry, rw, rh, color); });
}

void VirtualFramebuffer::draw_rect(int32_t rx, int32_t ry, int32_t w, int32_t h,
                                   uint32_t color) {
  paint(VFB_VIEW, rx, ry, w, h,
        [&](const Canvas &c) { rect_outline(c, rx, ry, w, h, color); });
}

void VirtualFramebuffer::draw_line(int32_t x0, int32_t y0, int32_t x1,
                                   int32_t y1, uint32_t color) {
  paint(VFB_VIEW, std::min(x0, x1) - 1, std::min(y0, y1) - 1,
        std::abs(x1 - x0) + 3, std::abs(y1 - y0) + 3,
        [&](const Canvas &c) { line(c, x0, y0, x1, y1, color); });
}

void VirtualFramebuffer::draw_circle(int32_t xc, int32_t yc, int32_t r,
                                     uint32_t color) {
  paint(VFB_VIEW, xc - r - 1, yc - r - 1, 2 * r + 3, 2 * r + 3,
        [&](const Canvas &c) { circle(c, xc, yc, r, color, 255, true); });
}

void VirtualFramebuffer::fill_circle(int32_t xc, int32_t yc, int32_t r,
                                     uint32_t color) {
  paint(VFB_VIEW, xc - r - 1, yc - r - 1, 2 * r + 3, 2 * r + 3,
        [&](const Canvas &c) { circle(c, xc, yc, r, color, 255, false); });
}

void VirtualFramebuffer::fill_rect_alpha(int32_t rx, int32_t ry, int32_t w,
                                         int32_t h, uint32_t color,
                                         uint8_t alpha) {
  if (alpha == 0)
    return;
  if (alpha == 255)
    return fill_rect(rx, ry, w, h, color);
  paint(VFB_VIEW, rx, ry, w, h, [&](const Canvas &c) {
    Box b;
    if (!box(c, rx, ry, int64_t(rx) + w, int64_t(ry) + h, b))
      return;
    for (int32_t y = b.t; y < b.b; ++y)
      span_fill(row_ptr(c, y) + b.l, b.r - b.l, color, alpha);
  });
}

void VirtualFramebuffer::fill_circle_alpha(int32_t xc, int32_t yc, int32_t r,
                                           uint32_t color, uint8_t alpha) {
  if (alpha == 0)
    return;
  paint(VFB_VIEW, xc - r - 1, yc - r - 1, 2 * r + 3, 2 * r + 3,
        [&](const Canvas &c) { circle(c, xc, yc, r, color, alpha, false); });
}

int VirtualFramebuffer::draw_string(int32_t x, int32_t y,
                                    float font_size_pixels, const char *text,
                                    uint32_t color) {
  if (TrueTypeFont *font = Raster::default_font())
    return draw_text_truetype(x, y, font_size_pixels, *font, text, color);
  int end = x + std::max(1, int(font_size_pixels + 0.5f)) * int(strlen(text));
  paint(VFB_VIEW, 0, 0, width_, height_, [&](const Canvas &c) {
    end = text8(c, x, y, font_size_pixels, text, color);
  });
  return end;
}

void VirtualFramebuffer::draw_glyph(int32_t gx, int32_t gy, char ch,
                                    uint32_t color) {
  paint(VFB_VIEW, gx, gy, 32, 32, [&](const Canvas &c) {
    if (!glyph_ttf(c, gx, gy, ch, color, 0, false))
      glyph(c, gx, gy, ch, color, 0, false);
  });
}

void VirtualFramebuffer::draw_glyph_bg(int32_t gx, int32_t gy, char ch,
                                       uint32_t fg, uint32_t bg) {
  paint(VFB_VIEW, gx, gy, 32, 32, [&](const Canvas &c) {
    if (!glyph_ttf(c, gx, gy, ch, fg, bg, true))
      glyph(c, gx, gy, ch, fg, bg, true);
  });
}

void VirtualFramebuffer::blit(const uint32_t *src, int32_t bx, int32_t by,
                              uint32_t bw, uint32_t bh, uint32_t src_pitch) {
  paint(VFB_VIEW, bx, by, bw, bh, [&](const Canvas &c) {
    Box b;
    if (!box(c, bx, by, int64_t(bx) + bw, int64_t(by) + bh, b))
      return;
    const uint8_t *srow = reinterpret_cast<const uint8_t *>(src) +
                          size_t(b.t - by) * src_pitch +
                          size_t(b.l - bx) * sizeof(uint32_t);
    const size_t bytes = size_t(b.r - b.l) * sizeof(uint32_t);
    for (int32_t y = b.t; y < b.b; ++y, srow += src_pitch)
      memcpy(row_ptr(c, y) + b.l, srow, bytes);
  });
}

void VirtualFramebuffer::blit_transparent(const uint32_t *src, int32_t bx,
                                          int32_t by, uint32_t bw, uint32_t bh,
                                          uint32_t src_pitch,
                                          uint32_t transparent_color) {
  paint(VFB_VIEW, bx, by, bw, bh, [&](const Canvas &c) {
    blit_pixels(c, bx, by, bw, bh, src, src_pitch,
                [transparent_color](uint32_t s, uint32_t d) {
                  return s == transparent_color ? d : s;
                });
  });
}

void VirtualFramebuffer::blit_alpha(const uint32_t *src, int32_t bx, int32_t by,
                                    uint32_t bw, uint32_t bh,
                                    uint32_t src_pitch, uint8_t global_alpha) {
  if (global_alpha == 0)
    return;
  paint(VFB_VIEW, bx, by, bw, bh, [&](const Canvas &c) {
    blit_pixels(c, bx, by, bw, bh, src, src_pitch,
                [global_alpha](uint32_t s, uint32_t d) {
                  return Raster::blend(s, d, global_alpha);
                });
  });
}

void VirtualFramebuffer::draw_arc(int32_t xc, int32_t yc, int32_t r,
                                  int32_t start_angle, int32_t end_angle,
                                  uint32_t color) {
  paint(VFB_VIEW, xc - r - 1, yc - r - 1, 2 * r + 3, 2 * r + 3,
        [&](const Canvas &c) {
          walk_arc(xc, yc, r, start_angle, end_angle,
                   [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
                     line(c, x0, y0, x1, y1, color);
                   });
        });
}

void VirtualFramebuffer::fill_sector(int32_t xc, int32_t yc, int32_t r,
                                     int32_t start_angle, int32_t end_angle,
                                     uint32_t color) {
  paint(VFB_VIEW, xc - r - 1, yc - r - 1, 2 * r + 3, 2 * r + 3,
        [&](const Canvas &c) {
          sector(c, xc, yc, r, start_angle, end_angle, color);
        });
}

void VirtualFramebuffer::fill_triangle(int32_t x0, int32_t y0, int32_t x1,
                                       int32_t y1, int32_t x2, int32_t y2,
                                       uint32_t color) {
  const int32_t l = std::min({x0, x1, x2}) - 1, t = std::min({y0, y1, y2}) - 1;
  paint(VFB_VIEW, l, t, std::max({x0, x1, x2}) + 2 - l,
        std::max({y0, y1, y2}) + 2 - t,
        [&](const Canvas &c) { triangle(c, x0, y0, x1, y1, x2, y2, color); });
}

void VirtualFramebuffer::draw_rounded_rect(int32_t rx, int32_t ry, int32_t w,
                                           int32_t h, int32_t radius,
                                           uint32_t color) {
  paint(VFB_VIEW, rx - 1, ry - 1, w + 2, h + 2, [&](const Canvas &c) {
    rounded_outline(c, rx, ry, w, h, radius, color);
  });
}

void VirtualFramebuffer::fill_rounded_rect(int32_t rx, int32_t ry, int32_t w,
                                           int32_t h, int32_t radius,
                                           uint32_t color) {
  paint(VFB_VIEW, rx, ry, w, h, [&](const Canvas &c) {
    Raster::fill_rounded_rect(c, rx, ry, w, h, radius, color);
  });
}

int VirtualFramebuffer::draw_text_truetype(int32_t x, int32_t y,
                                           float font_size_pixels,
                                           TrueTypeFont &font, const char *text,
                                           uint32_t color) {
  int end = 0;
  bool drawn = false;
  paint(VFB_VIEW, x, y, width_, int32_t(font_size_pixels * 2),
        [&](const Canvas &c) {
          end = Raster::text(c, font, x, y, font_size_pixels, text, color);
          drawn = true;
        });
  return drawn ? end : x + font.get_text_width(font_size_pixels, text);
}

#undef VFB_VIEW

} // namespace Kernel::Graphics
