// vfb.cpp - rasteriser + command encoder.
//
//   window mode (open_window): primitives are ENCODED into the window's SPSC
//     command ring (a few stores, no lock, no wait). The compositor thread
//     decodes and rasterises them (Raster::execute).
//   screen mode (init):        primitives rasterise immediately at (x,y) on
//     the raw framebuffer.
//   memory mode (attach):      primitives rasterise immediately into the
//     caller's buffer; flush() queues that buffer to the window (by reference).
//
// Anti-aliasing is 2x2 supersampling on the lattice (4x+1|3, 4y+1|3).
// Discs, rings, triangles and sectors are scan-converted from per-sample-row
// intervals: only edge pixels are evaluated, interiors are plain stores.
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

namespace std {
static inline int abs(int x) { return x < 0 ? -x : x; }
} // namespace std

namespace Kernel::Graphics {
namespace {

enum Op : uint32_t {
  OpPad = Cmd::kPad,
  OpPixel, OpFillRect, OpRect, OpLine, OpCircle, OpFillCircle, OpFillRectA,
  OpFillCircleA, OpGlyph, OpGlyphBg, OpText, OpBlit, OpArc, OpSector,
  OpFillTri, OpRRect, OpFillRRect
};
enum : uint32_t { BlitOpaque, BlitKey, BlitAlpha };
constexpr uint32_t kMaxText = 1024;

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
  if (n < 8) {
    while (n--)
      *p++ = v;
    return;
  }
  const uint64_t v64 = uint64_t(v) | (uint64_t(v) << 32);
  size_t n64 = n / 2;
  if (n64)
    asm volatile("rep stosq" : "+D"(p), "+c"(n64) : "a"(v64) : "memory");
  if (n & 1)
    *p = v;
}

// Exact per-channel (fg*a + bg*(255-a)) / 255 with rounding; R and B share
// one multiply. Valid for any a, callers handle the 0 / 255 shortcuts.
inline uint32_t mix(uint32_t fg, uint32_t bg, uint32_t a) {
  const uint32_t ia = 255 - a;
  uint32_t rb = (fg & 0x00FF00FFu) * a + (bg & 0x00FF00FFu) * ia + 0x00800080u;
  rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
  uint32_t g = ((fg >> 8) & 255) * a + ((bg >> 8) & 255) * ia + 128;
  g = (g + (g >> 8)) >> 8;
  return 0xFF000000u | rb | (g << 8);
}

// Samples in 0..4 scaled by alpha; fully covered pixels are written blind.
inline void put_cov(const Canvas &c, int32_t x, int32_t y, uint32_t color,
                    uint32_t samples, uint32_t alpha = 255) {
  const uint32_t cov = (samples * alpha + 2) / 4;
  if (!cov)
    return;
  uint32_t &p = row_ptr(c, y)[x];
  p = cov >= 255 ? color : mix(color, p, cov);
}

inline void span_fill(uint32_t *p, int64_t n, uint32_t color, uint32_t alpha) {
  if (n <= 0 || alpha == 0)
    return;
  if (alpha >= 255)
    return fill32(p, color, size_t(n));

  const uint32_t ia = 255 - alpha;
  const uint32_t fg_rb = (color & 0x00FF00FFu) * alpha + 0x00800080u;
  const uint32_t fg_g = ((color >> 8) & 255) * alpha + 128;

  for (int64_t i = 0; i < n; ++i) {
    const uint32_t bg = p[i];
    uint32_t rb = fg_rb + (bg & 0x00FF00FFu) * ia;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    uint32_t g = fg_g + ((bg >> 8) & 255) * ia;
    g = (g + (g >> 8)) >> 8;
    p[i] = 0xFF000000u | rb | (g << 8);
  }
}

// ---- 8x8 bitmap text ------------------------------------------------------

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

// ---- sample-interval scan conversion -------------------------------------

// Samples (all integers; only odd ones exist) in [lo, hi]; empty if lo > hi.
struct Iv {
  int64_t lo = 1, hi = 0;
  bool empty() const { return lo > hi; }
};
struct Iv2 {
  Iv a, b;
};

inline int64_t isqrt(int64_t v) {
  if (v < 2)
    return v;
  return static_cast<int64_t>(__builtin_sqrt(static_cast<double>(v)));
}

inline int64_t floordiv(int64_t a, int64_t b) {
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

  int64_t fa = 1, fz = 0, ha = 1, hz = 0;
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
  const bool has_solid = (fa <= fz);
  const bool has_hole = (ha <= hz);

  if (has_solid) {
    const int64_t s_start = std::max(a, fa);
    const int64_t s_end = std::min(z, fz);
    for (int64_t x = a; x < s_start; ++x)
      put_cov(c, int32_t(x), y, color, samples_at(int32_t(x), s), alpha);
    if (s_start <= s_end)
      span_fill(row + s_start, s_end - s_start + 1, color, alpha);
    for (int64_t x = std::max(a, s_end + 1); x <= z; ++x)
      put_cov(c, int32_t(x), y, color, samples_at(int32_t(x), s), alpha);
  } else if (has_hole) {
    const int64_t h_start = std::max(a, ha);
    const int64_t h_end = std::min(z, hz);
    for (int64_t x = a; x < h_start; ++x)
      put_cov(c, int32_t(x), y, color, samples_at(int32_t(x), s), alpha);
    for (int64_t x = std::max(a, h_end + 1); x <= z; ++x)
      put_cov(c, int32_t(x), y, color, samples_at(int32_t(x), s), alpha);
  } else {
    for (int64_t x = a; x <= z; ++x)
      put_cov(c, int32_t(x), y, color, samples_at(int32_t(x), s), alpha);
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
      } else if (!big) {
        Iv v = d;
        halfplane(v, -d0y, d0x * dy + d0y * cx);
        halfplane(v, d1y, -d1y * cx - dy * d1x);
        s[k].a = v;
      } else {
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

// ---- lines ---------------------------------------------------------------

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
  if (x0 == x1 || y0 == y1) {
    Raster::fill_rect(c, std::min(x0, x1), std::min(y0, y1),
                      std::abs(x1 - x0) + 1, std::abs(y1 - y0) + 1, color);
    return;
  }
  int32_t dx = std::abs(x1 - x0);
  int32_t dy = std::abs(y1 - y0);
  int32_t sx = (x0 < x1) ? 1 : -1;
  int32_t sy = (y0 < y1) ? 1 : -1;
  int32_t err = dx - dy;
  bool fully_inside = (std::min(x0, x1) >= c.l && std::max(x0, x1) < c.r &&
                       std::min(y0, y1) >= c.t && std::max(y0, y1) < c.b);

  if (fully_inside) {
    uint32_t *ptr = row_ptr(c, y0) + x0;
    int32_t stride = c.stride;
    while (true) {
      *ptr = color;
      if (x0 == x1 && y0 == y1)
        break;
      int32_t e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x0 += sx;
        ptr += sx; // Step horizontally in memory
      }
      if (e2 < dx) {
        err += dx;
        y0 += sy;
        ptr += sy * stride;
      }
    }
  } else {
    while (true) {
      if (x0 >= c.l && x0 < c.r && y0 >= c.t && y0 < c.b) {
        row_ptr(c, y0)[x0] = color;
      }
      if (x0 == x1 && y0 == y1)
        break;
      int32_t e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x0 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y0 += sy;
      }
    }
  }
}

template <typename Draw>
void walk_arc(int32_t xc, int32_t yc, int32_t r, int32_t start, int32_t end,
              Draw draw) {
  if (r <= 0 || start >= end)
    return;
  const int32_t step = (r < 15) ? 6 : (r < 30) ? 3 : (r < 60) ? 2 : 1;
  int32_t px = xc + r * Kernel::Lib::Math::fixed_cos(start) / 1024;
  int32_t py = yc + r * Kernel::Lib::Math::fixed_sin(start) / 1024;
  for (int32_t deg = start + step; deg < end; deg += step) {
    const int32_t x = xc + r * Kernel::Lib::Math::fixed_cos(deg) / 1024;
    const int32_t y = yc + r * Kernel::Lib::Math::fixed_sin(deg) / 1024;
    draw(px, py, x, y);
    px = x;
    py = y;
  }
  const int32_t x = xc + r * Kernel::Lib::Math::fixed_cos(end) / 1024;
  const int32_t y = yc + r * Kernel::Lib::Math::fixed_sin(end) / 1024;
  draw(px, py, x, y);
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


void blit_any(const Canvas &c, int64_t bx, int64_t by, uint32_t bw,
              uint32_t bh, const uint32_t *src, uint32_t sp, uint32_t mode,
              uint32_t param) {
  Box b;
  if (!box(c, bx, by, bx + bw, by + bh, b))
    return;
  const uint8_t *srow = reinterpret_cast<const uint8_t *>(src) +
                        size_t(b.t - by) * sp + size_t(b.l - bx) * 4;
  const int32_t n = b.r - b.l;
  if (mode == BlitOpaque) {
    for (int32_t y = b.t; y < b.b; ++y, srow += sp) {
      uint32_t *d = row_ptr(c, y) + b.l;
      memcpy(d, srow, size_t(n) * 4);
    }
  } else if (mode == BlitKey) {
    for (int32_t y = b.t; y < b.b; ++y, srow += sp) {
      const auto *s = reinterpret_cast<const uint32_t *>(srow);
      uint32_t *d = row_ptr(c, y) + b.l;
      for (int32_t x = 0; x < n; ++x) {
        if (s[x] != param)
          d[x] = s[x];
      }
    }
  } else { // BlitAlpha
    const uint32_t alpha = param;
    if (alpha == 0)
      return;
    if (alpha >= 255) {
      for (int32_t y = b.t; y < b.b; ++y, srow += sp) {
        uint32_t *d = row_ptr(c, y) + b.l;
        memcpy(d, srow, size_t(n) * 4);
      }
      return;
    }
    const uint32_t ia = 255 - alpha;
    for (int32_t y = b.t; y < b.b; ++y, srow += sp) {
      const auto *s = reinterpret_cast<const uint32_t *>(srow);
      uint32_t *d = row_ptr(c, y) + b.l;
      for (int32_t x = 0; x < n; ++x) {
        const uint32_t fg = s[x];
        const uint32_t bg = d[x];
        uint32_t rb = (fg & 0x00FF00FFu) * alpha + (bg & 0x00FF00FFu) * ia + 0x00800080u;
        rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
        uint32_t g = ((fg >> 8) & 255) * alpha + ((bg >> 8) & 255) * ia + 128;
        g = (g + (g >> 8)) >> 8;
        d[x] = 0xFF000000u | rb | (g << 8);
      }
    }
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

// ---- submission: encode into the ring, or rasterise right now -------------

struct Target {
  Cmd::Ring *q;
  uint32_t *buf;
  uint32_t pitch;
  int32_t x, y;
  uint32_t w, h;
};

// Canvas for the immediate (screen / memory) modes.
bool direct_canvas(const Target &t, Canvas &c) {
  if (t.buf) {
    c = {t.buf, ptrdiff_t(t.pitch / 4), 0, 0, int32_t(t.w), int32_t(t.h)};
    return t.w && t.h;
  }
  const Canvas s = Raster::screen();
  if (!s.origin)
    return false;
  c.origin = s.origin + ptrdiff_t(t.y) * s.stride + t.x;
  c.stride = s.stride;
  c.l = std::max<int32_t>(0, -t.x);
  c.t = std::max<int32_t>(0, -t.y);
  c.r = int32_t(std::min<int64_t>(t.w, int64_t(s.r) - t.x));
  c.b = int32_t(std::min<int64_t>(t.h, int64_t(s.b) - t.y));
  return c.l < c.r && c.t < c.b;
}

template <class... A>
inline void submit(const Target &t, uint32_t op, A... a) {
  constexpr uint32_t n = 1 + sizeof...(A);
  uint32_t tmp[n];
  uint32_t *p = tmp;
  if (t.q && !(p = Cmd::begin(t.q, n))) [[unlikely]]
    return;
  Canvas c;
  if (!t.q && !direct_canvas(t, c))
    return;
  p[0] = op | n << 8;
  uint32_t i = 1;
  ((p[i++] = uint32_t(a)), ...);
  if (t.q)
    Cmd::end(t.q);
  else
    Raster::execute(c, tmp);
}

int submit_text(const Target &t, int32_t x, int32_t y, float size,
                TrueTypeFont *font, const char *s, uint32_t color) {
  const int gs = std::max(1, int(size + 0.5f));
  const int end = font ? x + font->get_text_width(size, s)
                       : x + gs * int(strlen(s));
  if (t.q) {
    uint32_t len = uint32_t(strlen(s));
    if (len > kMaxText)
      len = kMaxText;
    const uint32_t words = 8 + (len + 4) / 4; // chars + NUL
    if (uint32_t *p = Cmd::begin(t.q, words)) {
      uint32_t sb;
      __builtin_memcpy(&sb, &size, 4);
      const uint64_t f = uint64_t(uintptr_t(font));
      p[0] = OpText | words << 8;
      p[1] = uint32_t(x);
      p[2] = uint32_t(y);
      p[3] = sb;
      p[4] = color;
      p[5] = uint32_t(f);
      p[6] = uint32_t(f >> 32);
      p[7] = len;
      p[words - 1] = 0;
      memcpy(p + 8, s, len);
      Cmd::end(t.q);
    }
  } else if (Canvas c; direct_canvas(t, c)) {
    if (font)
      Raster::text(c, *font, x, y, size, s, color);
    else
      text8(c, x, y, size, s, color);
  }
  return end;
}

void submit_blit(const Target &t, int32_t bx, int32_t by, uint32_t bw,
                 uint32_t bh, const uint32_t *src, uint32_t sp, uint32_t mode,
                 uint32_t param) {
  if (!bw || !bh)
    return;
  if (t.q)
    Cmd::blit(t.q, bx, by, bw, bh, src, sp, mode, param);
  else if (Canvas c; direct_canvas(t, c))
    blit_any(c, bx, by, bw, bh, src, sp, mode, param);
}

} // namespace

namespace Cmd {
void blit(Ring *q, int32_t x, int32_t y, uint32_t w, uint32_t h,
          const uint32_t *src, uint32_t pitch, uint32_t mode, uint32_t param) {
  const bool inl = uint64_t(w) * h <= kInlinePixels;
  const uint32_t n = 10 + (inl ? w * h : 0);
  uint32_t *p = begin(q, n);
  if (!p)
    return;
  const uint64_t ptr = inl ? 0 : uint64_t(uintptr_t(src));
  p[0] = OpBlit | n << 8;
  p[1] = uint32_t(x);
  p[2] = uint32_t(y);
  p[3] = w;
  p[4] = h;
  p[5] = pitch;
  p[6] = mode;
  p[7] = param;
  p[8] = uint32_t(ptr);
  p[9] = uint32_t(ptr >> 32);
  if (inl)
    for (uint32_t r = 0; r < h; ++r)
      memcpy(p + 10 + r * w,
             reinterpret_cast<const uint8_t *>(src) + size_t(r) * pitch,
             size_t(w) * 4);
  end(q);
}
} // namespace Cmd

namespace Raster {

TrueTypeFont *default_font() {
  static TrueTypeFont font;
  static int state = 0;
  if (!state)
    state = (OpenFont::load_embedded_named(font, "GeistMono.ttf") ||
             OpenFont::load_preferred(font, "GeistMono.ttf", "Geist.ttf")) &&
                    font.is_valid()
                ? 1
                : 2;
  return state == 1 ? &font : nullptr;
}

Canvas screen() {
  static Canvas s{};
  if (!s.origin && Kernel::Drivers::Framebuffer::valid()) {
    const auto info = Kernel::Drivers::Framebuffer::get_info();
    s = {reinterpret_cast<uint32_t *>(uintptr_t(info.address)),
         ptrdiff_t(info.pitch / 4), 0, 0,
         int32_t(Kernel::Drivers::Framebuffer::width()),
         int32_t(Kernel::Drivers::Framebuffer::height())};
  }
  return s;
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
  if (ptrdiff_t(n) == c.stride)
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
  uint32_t *sub = row_ptr(c, c.t) + c.l;
  return font.render_text(sub, uint32_t(c.r - c.l), uint32_t(c.b - c.t),
                          uint32_t(c.stride * 4), x - c.l, y - c.t, size, str,
                          color) +
         c.l;
}

void glyph8(const Canvas &c, int32_t x, int32_t y, char ch, uint32_t color) {
  glyph(c, x, y, ch, color, 0, false);
}

// Decodes one record and draws it clipped to c.
void execute(const Canvas &c, const uint32_t *r) {
  const int32_t *a = reinterpret_cast<const int32_t *>(r) + 1;
  const uint32_t *u = r + 1;
  switch (r[0] & 0xFFu) {
  case OpPixel: {
    const int x = int16_t(u[0]), y = int16_t(u[0] >> 16);
    if (x >= c.l && x < c.r && y >= c.t && y < c.b)
      row_ptr(c, y)[x] = u[1];
    break;
  }
  case OpFillRect:
    fill_rect(c, a[0], a[1], a[2], a[3], u[4]);
    break;
  case OpRect:
    rect_outline(c, a[0], a[1], a[2], a[3], u[4]);
    break;
  case OpLine:
    line(c, a[0], a[1], a[2], a[3], u[4]);
    break;
  case OpCircle:
    circle(c, a[0], a[1], a[2], u[3], 255, true);
    break;
  case OpFillCircle:
    circle(c, a[0], a[1], a[2], u[3], 255, false);
    break;
  case OpFillRectA: {
    Box b;
    if (box(c, a[0], a[1], int64_t(a[0]) + a[2], int64_t(a[1]) + a[3], b))
      for (int32_t y = b.t; y < b.b; ++y)
        span_fill(row_ptr(c, y) + b.l, b.r - b.l, u[4], u[5]);
    break;
  }
  case OpFillCircleA:
    circle(c, a[0], a[1], a[2], u[3], u[4], false);
    break;
  case OpGlyph:
    if (!glyph_ttf(c, a[0], a[1], char(u[2]), u[3], 0, false))
      glyph(c, a[0], a[1], char(u[2]), u[3], 0, false);
    break;
  case OpGlyphBg:
    if (!glyph_ttf(c, a[0], a[1], char(u[2]), u[3], u[4], true))
      glyph(c, a[0], a[1], char(u[2]), u[3], u[4], true);
    break;
  case OpText: {
    float size;
    __builtin_memcpy(&size, &r[3], 4);
    TrueTypeFont *font = reinterpret_cast<TrueTypeFont *>(
        uintptr_t(uint64_t(r[5]) | uint64_t(r[6]) << 32));
    const char *s = reinterpret_cast<const char *>(r + 8);
    if (font)
      text(c, *font, a[0], a[1], size, s, r[4]);
    else
      text8(c, a[0], a[1], size, s, r[4]);
    break;
  }
  case OpBlit: {
    const uint64_t ptr = uint64_t(r[8]) | uint64_t(r[9]) << 32;
    const uint32_t *src =
        ptr ? reinterpret_cast<const uint32_t *>(uintptr_t(ptr)) : r + 10;
    blit_any(c, a[0], a[1], u[2], u[3], src, ptr ? u[4] : u[2] * 4, u[5],
             u[6]);
    break;
  }
  case OpArc:
    walk_arc(a[0], a[1], a[2], a[3], a[4],
             [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
               line(c, x0, y0, x1, y1, u[5]);
             });
    break;
  case OpSector:
    sector(c, a[0], a[1], a[2], a[3], a[4], u[5]);
    break;
  case OpFillTri:
    triangle(c, a[0], a[1], a[2], a[3], a[4], a[5], u[6]);
    break;
  case OpRRect:
    rounded_outline(c, a[0], a[1], a[2], a[3], a[4], u[5]);
    break;
  case OpFillRRect:
    fill_rounded_rect(c, a[0], a[1], a[2], a[3], a[4], u[5]);
    break;
  default:
    break;
  }
}

bool get_record_bounds(const uint32_t *r, int32_t &l, int32_t &t, int32_t &rr,
                       int32_t &b) {
  const int32_t *a = reinterpret_cast<const int32_t *>(r) + 1;
  const uint32_t *u = r + 1;
  switch (r[0] & 0xFFu) {
  case OpPixel: {
    l = int16_t(u[0]);
    t = int16_t(u[0] >> 16);
    rr = l + 1;
    b = t + 1;
    return true;
  }
  case OpFillRect:
  case OpRect:
  case OpFillRectA:
  case OpRRect:
  case OpFillRRect:
    l = a[0];
    t = a[1];
    rr = l + a[2];
    b = t + a[3];
    return true;
  case OpLine:
    l = std::min(a[0], a[2]);
    t = std::min(a[1], a[3]);
    rr = std::max(a[0], a[2]) + 1;
    b = std::max(a[1], a[3]) + 1;
    return true;
  case OpCircle:
  case OpFillCircle:
  case OpFillCircleA:
  case OpArc:
  case OpSector:
    l = a[0] - a[2] - 1;
    t = a[1] - a[2] - 1;
    rr = a[0] + a[2] + 2;
    b = a[1] + a[2] + 2;
    return true;
  case OpGlyph:
  case OpGlyphBg:
    l = a[0];
    t = a[1];
    rr = l + 16;
    b = t + 18;
    return true;
  case OpBlit:
    l = a[0];
    t = a[1];
    rr = l + int32_t(u[2]);
    b = t + int32_t(u[3]);
    return true;
  case OpFillTri:
    l = std::min({a[0], a[2], a[4]});
    t = std::min({a[1], a[3], a[5]});
    rr = std::max({a[0], a[2], a[4]}) + 1;
    b = std::max({a[1], a[3], a[5]}) + 1;
    return true;
  default:
    return false;
  }
}

} // namespace Raster

// ============================================================================
// VirtualFramebuffer
// ============================================================================
#define TGT Target{ring_, buffer_, pitch_, x_, y_, width_, height_}

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
    ring_ = other.ring_;
    other.destroy();
  }
  return *this;
}

bool VirtualFramebuffer::init(int32_t x, int32_t y, uint32_t width,
                              uint32_t height) {
  return attach(nullptr, x, y, width, height);
}

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

bool VirtualFramebuffer::open_window(uint32_t requested_width,
                                     uint32_t requested_height, uint32_t,
                                     uint32_t) {
  using namespace WindowManager;
  const uint64_t tid = Kernel::Scheduler::current_thread_id();
  uint32_t w = 0, h = 0;
  Cmd::Ring *q = nullptr;
  if (!request_window_size(tid, requested_width, requested_height, &w, &h) ||
      !(q = queue_of(tid)) || !init(0, 0, w, h)) {
    destroy();
    return false;
  }
  ring_ = q;
  return true;
}

bool VirtualFramebuffer::has_focus() const {
  return Kernel::Graphics::WindowManager::has_keyboard_focus(
      Kernel::Scheduler::current_thread_id());
}

void VirtualFramebuffer::destroy() {
  buffer_ = nullptr;
  ring_ = nullptr;
  initialized_ = false;
  width_ = height_ = 0;
}

// Only memory mode needs publishing; window and screen modes already are.
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
  submit(TGT, OpPixel, uint32_t(uint16_t(px)) | uint32_t(py) << 16, color);
}

void VirtualFramebuffer::fill(uint32_t color) {
  submit(TGT, OpFillRect, 0, 0, width_, height_, color);
}

void VirtualFramebuffer::fill_rect(int32_t rx, int32_t ry, int32_t rw,
                                   int32_t rh, uint32_t color) {
  submit(TGT, OpFillRect, rx, ry, rw, rh, color);
}

void VirtualFramebuffer::draw_rect(int32_t rx, int32_t ry, int32_t w, int32_t h,
                                   uint32_t color) {
  submit(TGT, OpRect, rx, ry, w, h, color);
}

void VirtualFramebuffer::draw_line(int32_t x0, int32_t y0, int32_t x1,
                                   int32_t y1, uint32_t color) {
  submit(TGT, OpLine, x0, y0, x1, y1, color);
}

void VirtualFramebuffer::draw_circle(int32_t xc, int32_t yc, int32_t r,
                                     uint32_t color) {
  submit(TGT, OpCircle, xc, yc, r, color);
}

void VirtualFramebuffer::fill_circle(int32_t xc, int32_t yc, int32_t r,
                                     uint32_t color) {
  submit(TGT, OpFillCircle, xc, yc, r, color);
}

void VirtualFramebuffer::fill_rect_alpha(int32_t rx, int32_t ry, int32_t w,
                                         int32_t h, uint32_t color,
                                         uint8_t alpha) {
  if (alpha == 0)
    return;
  if (alpha == 255)
    return submit(TGT, OpFillRect, rx, ry, w, h, color);
  submit(TGT, OpFillRectA, rx, ry, w, h, color, alpha);
}

void VirtualFramebuffer::fill_circle_alpha(int32_t xc, int32_t yc, int32_t r,
                                           uint32_t color, uint8_t alpha) {
  if (alpha == 0)
    return;
  submit(TGT, OpFillCircleA, xc, yc, r, color, alpha);
}

int VirtualFramebuffer::draw_string(int32_t x, int32_t y,
                                    float font_size_pixels, const char *text,
                                    uint32_t color) {
  return submit_text(TGT, x, y, font_size_pixels, Raster::default_font(), text,
                     color);
}

int VirtualFramebuffer::draw_text_truetype(int32_t x, int32_t y,
                                           float font_size_pixels,
                                           TrueTypeFont &font,
                                           const char *text, uint32_t color) {
  // The font must outlive the compositor's execution of this record.
  return submit_text(TGT, x, y, font_size_pixels, &font, text, color);
}

void VirtualFramebuffer::draw_glyph(int32_t gx, int32_t gy, char ch,
                                    uint32_t color) {
  submit(TGT, OpGlyph, gx, gy, uint8_t(ch), color);
}

void VirtualFramebuffer::draw_glyph_bg(int32_t gx, int32_t gy, char ch,
                                       uint32_t fg, uint32_t bg) {
  submit(TGT, OpGlyphBg, gx, gy, uint8_t(ch), fg, bg);
}

void VirtualFramebuffer::blit(const uint32_t *src, int32_t bx, int32_t by,
                              uint32_t bw, uint32_t bh, uint32_t src_pitch) {
  submit_blit(TGT, bx, by, bw, bh, src, src_pitch, BlitOpaque, 0);
}

void VirtualFramebuffer::blit_transparent(const uint32_t *src, int32_t bx,
                                          int32_t by, uint32_t bw, uint32_t bh,
                                          uint32_t src_pitch,
                                          uint32_t transparent_color) {
  submit_blit(TGT, bx, by, bw, bh, src, src_pitch, BlitKey, transparent_color);
}

void VirtualFramebuffer::blit_alpha(const uint32_t *src, int32_t bx, int32_t by,
                                    uint32_t bw, uint32_t bh,
                                    uint32_t src_pitch, uint8_t global_alpha) {
  if (global_alpha == 0)
    return;
  submit_blit(TGT, bx, by, bw, bh, src, src_pitch,
              global_alpha == 255 ? BlitOpaque : BlitAlpha, global_alpha);
}

void VirtualFramebuffer::draw_arc(int32_t xc, int32_t yc, int32_t r,
                                  int32_t start_angle, int32_t end_angle,
                                  uint32_t color) {
  submit(TGT, OpArc, xc, yc, r, start_angle, end_angle, color);
}

void VirtualFramebuffer::fill_sector(int32_t xc, int32_t yc, int32_t r,
                                     int32_t start_angle, int32_t end_angle,
                                     uint32_t color) {
  submit(TGT, OpSector, xc, yc, r, start_angle, end_angle, color);
}

void VirtualFramebuffer::fill_triangle(int32_t x0, int32_t y0, int32_t x1,
                                       int32_t y1, int32_t x2, int32_t y2,
                                       uint32_t color) {
  submit(TGT, OpFillTri, x0, y0, x1, y1, x2, y2, color);
}

void VirtualFramebuffer::draw_rounded_rect(int32_t rx, int32_t ry, int32_t w,
                                           int32_t h, int32_t radius,
                                           uint32_t color) {
  submit(TGT, OpRRect, rx, ry, w, h, radius, color);
}

void VirtualFramebuffer::fill_rounded_rect(int32_t rx, int32_t ry, int32_t w,
                                           int32_t h, int32_t radius,
                                           uint32_t color) {
  submit(TGT, OpFillRRect, rx, ry, w, h, radius, color);
}

#undef TGT

} // namespace Kernel::Graphics
