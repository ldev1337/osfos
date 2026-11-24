// vga_text.cpp - glyph atlas, diffing and cell painter for emulated VGA text.
#include <algorithm>
#include <stdlib.h>
#include <string.h>

#include <kernel/graphics/internal.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/simplefonts.h>
#include <kernel/graphics/truetype.h>
#include <kernel/graphics/vga_emu.h>
#include <kernel/scheduler.h>

namespace Kernel::Graphics::Vga {
namespace {

constexpr int W = int(kCellW), H = int(kCellH);

template <class T> inline T ld(const T &v) {
  return __atomic_load_n(&v, __ATOMIC_RELAXED);
}
template <class T> inline void stv(T &v, T x) {
  __atomic_store_n(&v, x, __ATOMIC_RELAXED);
}

// ---- tuning -----------------------------------------------------------------
// Row (0 = top of the 16-line cell) on which the baseline of capital letters
// sits. IBM's 9x16 ROM font ends capitals on line 11 and lets descenders use
// 12..14.
constexpr int kBaselineRow = 11;
// Hardware timing at 70 Hz text mode: cursor toggles every 16 frames, the
// character-blink attribute every 32.
constexpr uint64_t kCursorHalfMs = 228, kBlinkHalfMs = 457;
constexpr int kCursorDefaultStart = 14, kCursorDefaultEnd = 15;

constexpr uint32_t kPalette[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA, 0xFFAA0000, 0xFFAA00AA,
    0xFFAA5500, 0xFFAAAAAA, 0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF};

// ---- CP437 -> Unicode (for the TrueType path)
// -----------------------------------
constexpr uint16_t kLow[32] = {
    0x0020, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC};
constexpr uint16_t kHigh[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA,
    0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6,
    0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC,
    0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC,
    0x00A1, 0x00AB, 0x00BB,
    // 0xB0..0xDF are generated procedurally; entries kept for completeness
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555,
    0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534,
    0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569,
    0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584,
    0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3,
    0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5,
    0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

uint32_t cp437_to_unicode(unsigned c) {
  if (c >= 0x20 && c < 0x7F)
    return c;
  if (c < 0x20)
    return kLow[c];
  if (c == 0x7F)
    return 0x2302;
  return kHigh[c - 0x80];
}

int utf8(uint32_t cp, char *o) {
  if (cp < 0x80) {
    o[0] = char(cp);
    o[1] = 0;
    return 1;
  }
  if (cp < 0x800) {
    o[0] = char(0xC0 | cp >> 6);
    o[1] = char(0x80 | (cp & 63));
    o[2] = 0;
    return 2;
  }
  o[0] = char(0xE0 | cp >> 12);
  o[1] = char(0x80 | ((cp >> 6) & 63));
  o[2] = char(0x80 | (cp & 63));
  o[3] = 0;
  return 3;
}

// ---- the atlas
// ----------------------------------------------------------------
uint8_t g_mask[256][H * W]; // 0..255 coverage, row-major 9x16
bool g_ready = false;

// Slightly steeper than linear (smoothstep): crisper stems at 9x16, closer to
// the hard-edged ROM font while keeping TrueType anti-aliasing on diagonals.
uint8_t contrast(unsigned v) {
  if (v >= 255)
    return 255;
  return uint8_t((v * v * (765 - 2 * v) + 32512) / 65025);
}

// -- procedural CP437 line / block / shade characters --
struct Arms {
  uint8_t u, d, l, r; // 0 none, 1 single, 2 double
};
constexpr Arms kBox[0xDA - 0xB3 + 1] = {
    {1, 1, 0, 0}, {1, 1, 1, 0}, {1, 1, 2, 0}, {2, 2, 1, 0}, {0, 2, 1, 0}, // B3
    {0, 1, 2, 0}, {2, 2, 2, 0}, {2, 2, 0, 0}, {0, 2, 2, 0}, {2, 0, 2, 0}, // B8
    {2, 0, 1, 0}, {1, 0, 2, 0}, {0, 1, 1, 0}, {1, 0, 0, 1}, {1, 0, 1, 1}, // BD
    {0, 1, 1, 1}, {1, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}, {1, 1, 0, 2}, // C2
    {2, 2, 0, 1}, {2, 0, 0, 2}, {0, 2, 0, 2}, {2, 0, 2, 2}, {0, 2, 2, 2}, // C7
    {2, 2, 0, 2}, {0, 0, 2, 2}, {2, 2, 2, 2}, {1, 0, 2, 2}, {2, 0, 1, 1}, // CC
    {0, 1, 2, 2}, {0, 2, 1, 1}, {2, 0, 0, 1}, {1, 0, 0, 2}, {0, 1, 0, 2}, // D1
    {0, 2, 0, 1}, {2, 2, 1, 1}, {1, 1, 2, 2}, {1, 0, 1, 0}, {0, 1, 0, 1}, // D6
};
static_assert(sizeof(kBox) / sizeof(kBox[0]) == 40, "box table");

void rect(uint8_t *m, int x0, int y0, int x1, int y1) {
  for (int y = std::max(y0, 0); y <= std::min(y1, H - 1); ++y)
    for (int x = std::max(x0, 0); x <= std::min(x1, W - 1); ++x)
      m[y * W + x] = 255;
}

// Every arm is a band (1 px single, 3 px double) reaching the cell centre plus
// half the perpendicular band; double bands then get their centre line carved
// out again, which yields correct corners, tees and crosses for all 40 glyphs.
// Arms run to the cell edge, so the 9th column joins neighbours (as the VGA
// "line graphics" column-9 duplication does).
void make_box(uint8_t *m, const Arms &a) {
  constexpr int cx = 4, cy = 8;
  const int hv = (a.u == 2 || a.d == 2), hh = (a.l == 2 || a.r == 2);
  uint8_t single[H * W] = {};
  memset(m, 0, size_t(H * W));
  auto arm = [&](int style, int x0, int y0, int x1, int y1) {
    if (!style)
      return;
    rect(m, x0, y0, x1, y1);
    if (style == 1)
      rect(single, x0, y0, x1, y1);
  };
  const int tu = a.u == 2, td = a.d == 2, tl = a.l == 2, tr = a.r == 2;
  arm(a.u, cx - tu, 0, cx + tu, cy + hh);
  arm(a.d, cx - td, cy - hh, cx + td, H - 1);
  arm(a.l, 0, cy - tl, cx + hv, cy + tl);
  arm(a.r, cx - hv, cy - tr, W - 1, cy + tr);
  auto clr = [&](int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
        m[y * W + x] = 0;
  };
  if (tu)
    clr(cx, 0, cx, cy);
  if (td)
    clr(cx, cy, cx, H - 1);
  if (tl)
    clr(0, cy, cx, cy);
  if (tr)
    clr(cx, cy, W - 1, cy);
  for (int i = 0; i < H * W; ++i)
    if (single[i])
      m[i] = 255;
}

void make_blocks() {
  auto sh = [&](int ch, int level) {
    uint8_t *m = g_mask[ch];
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        bool on;
        if (level == 1) // 25%
          on = (y & 1) == 0 && ((x + (y >> 1 & 1) * 2) & 3) == 0;
        else if (level == 2) // 50%
          on = ((x + y) & 1) == 0;
        else // 75%
          on = !((y & 1) == 0 && ((x + (y >> 1 & 1) * 2) & 3) == 0);
        m[y * W + x] = on ? 255 : 0;
      }
  };
  sh(0xB0, 1);
  sh(0xB1, 2);
  sh(0xB2, 3);
  memset(g_mask[0xDB], 0, H * W), rect(g_mask[0xDB], 0, 0, W - 1, H - 1);
  memset(g_mask[0xDC], 0, H * W), rect(g_mask[0xDC], 0, H / 2, W - 1, H - 1);
  memset(g_mask[0xDD], 0, H * W), rect(g_mask[0xDD], 0, 0, 3, H - 1);
  memset(g_mask[0xDE], 0, H * W), rect(g_mask[0xDE], 4, 0, W - 1, H - 1);
  memset(g_mask[0xDF], 0, H * W), rect(g_mask[0xDF], 0, 0, W - 1, H / 2 - 1);
  for (int c = 0xB3; c <= 0xDA; ++c)
    make_box(g_mask[c], kBox[c - 0xB3]);
}

// -- TrueType path --
bool build_ttf(TrueTypeFont &f) {
  constexpr int SW = 32, SH = 48, PAD = 12;
  static uint32_t scratch[SW * SH];
  auto render = [&](const char *s, float size, int x, int y) {
    for (uint32_t &p : scratch)
      p = 0xFF000000;
    f.render_text(scratch, SW, SH, SW * 4, x, y, size, s, 0xFFFFFFFF);
  };

  // 1. fit the size so a monospace advance is exactly one cell wide.
  const int adv160 = f.get_text_width(160.0f, "M");
  if (adv160 <= 0)
    return false;
  float size = float(W) * 160.0f / float(adv160);
  size = std::clamp(size, 8.0f, 20.0f);

  // 2. pin the baseline: measure where 'H' ends and shift every glyph so it
  //    ends on kBaselineRow.
  render("H", size, PAD, PAD);
  int bottom = -1;
  for (int y = SH - 1; y >= 0 && bottom < 0; --y)
    for (int x = 0; x < SW; ++x)
      if ((scratch[y * SW + x] & 0xFF) > 96) {
        bottom = y;
        break;
      }
  if (bottom < 0)
    return false;
  const int dy = kBaselineRow - (bottom - PAD);

  // 3. can the renderer take UTF-8? (a 2-byte sequence must be one advance)
  const bool u8 = f.get_text_width(160.0f, "\xC3\xA9") <
                  f.get_text_width(160.0f, "e") * 3 / 2;

  // 4. the font's "missing glyph" appearance, so tofu boxes can be blanked.
  uint8_t notdef[H * W] = {};
  bool have_notdef = false;
  if (u8) {
    render("\xEE\x80\x80", size, PAD, PAD + dy);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        notdef[y * W + x] = uint8_t(scratch[(PAD + y) * SW + PAD + x] & 0xFF);
        have_notdef |= notdef[y * W + x] > 32;
      }
  }

  for (int ch = 1; ch < 256; ++ch) {
    if ((ch >= 0xB0 && ch <= 0xDF) || ch == 0x20 || ch == 0xFF)
      continue;
    if (!u8 && (ch < 0x21 || ch > 0x7E))
      continue;
    char s[4];
    utf8(cp437_to_unicode(unsigned(ch)), s);
    render(s, size, PAD, PAD + dy);
    uint8_t *m = g_mask[ch];
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        m[y * W + x] = contrast(scratch[(PAD + y) * SW + PAD + x] & 0xFF);
    if (u8 && have_notdef && ch >= 0x80) {
      bool same = true;
      for (int i = 0; i < H * W && same; ++i) {
        const int d = int(contrast(notdef[i])) - int(m[i]);
        same = (d < 0 ? -d : d) < 8;
      }
      if (same)
        memset(m, 0, H * W);
    }
  }
  return true;
}

// -- 8x8 bitmap fallback when no TrueType font is available --
void build_bitmap() {
  for (int ch = 0x21; ch <= 0x7E; ++ch) {
    const uint8_t *bits = Font::Basic8x8[ch - 32];
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < 8; ++x)
        g_mask[ch][y * W + x] = (bits[y / 2] >> x) & 1 ? 255 : 0;
  }
}

inline uint32_t blend(uint32_t fg, uint32_t bg, uint32_t a) {
  const uint32_t ia = 255 - a;
  uint32_t rb = (fg & 0x00FF00FFu) * a + (bg & 0x00FF00FFu) * ia + 0x00800080u;
  rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
  uint32_t g = ((fg >> 8) & 255) * a + ((bg >> 8) & 255) * ia + 128;
  g = (g + (g >> 8)) >> 8;
  return 0xFF000000u | rb | (g << 8);
}

// key layout: bits 0..15 cell | 16 cursor here | 17 blink-hidden |
//             20..23 cursor start | 24..27 cursor end
void paint_cell(const Canvas &c, const Text &t, int px, int py, uint32_t key,
                int cx0, int cy0, int cx1, int cy1) {
  const int s = t.scale, cw = W * s, ch = H * s;
  const int xa = std::max(px, cx0), xb = std::min(px + cw, cx1);
  const int ya = std::max(py, cy0), yb = std::min(py + ch, cy1);
  if (xa >= xb || ya >= yb)
    return;

  const uint32_t a = (key >> 8) & 0xFF;
  const bool blink = ld(t.flags) & kFlagBlink;
  const uint32_t fg = t.palette[a & 15];
  const uint32_t bg = t.palette[blink ? (a >> 4) & 7 : a >> 4];
  const uint8_t *mask = g_mask[(key & 0x20000) ? 0x20 : (key & 0xFF)];
  const bool cur = key & 0x10000;
  const int cs = int((key >> 20) & 15), ce = int((key >> 24) & 15);

  for (int y = ya; y < yb; ++y) {
    const int my = (y - py) / s;
    uint32_t *row = c.origin + ptrdiff_t(y) * c.stride;
    const bool bar =
        cur && (cs <= ce ? (my >= cs && my <= ce) : (my >= cs || my <= ce));
    if (bar) {
      for (int x = xa; x < xb; ++x)
        row[x] = fg;
      continue;
    }
    const uint8_t *mrow = mask + my * W;
    int mx = (xa - px) / s, sub = (xa - px) % s;
    for (int x = xa; x < xb; ++x) {
      const uint32_t cov = mrow[mx];
      row[x] = cov == 0 ? bg : cov == 255 ? fg : blend(fg, bg, cov);
      if (++sub == s) {
        sub = 0;
        ++mx;
      }
    }
  }
}

} // namespace

// ---- public
// ---------------------------------------------------------------------------
void init_atlas() {
  if (g_ready)
    return;
  memset(g_mask, 0, sizeof g_mask);
  TrueTypeFont *font = Raster::default_font();
  if (!(font && build_ttf(*font)))
    build_bitmap();
  make_blocks();
  g_ready = true;
}

Text *alloc(uint32_t cols, uint32_t rows, uint32_t scale) {
  cols = std::clamp<uint32_t>(cols, 1, kMaxCols);
  rows = std::clamp<uint32_t>(rows, 1, kMaxRows);
  scale = std::clamp<uint32_t>(scale, 1, kMaxScale);
  const size_t n = size_t(cols) * rows;
  const size_t bytes =
      sizeof(Text) + n * (2 * sizeof(uint32_t) + sizeof(uint16_t));
  uint8_t *raw = static_cast<uint8_t *>(malloc(bytes));
  if (!raw)
    return nullptr;
  memset(raw, 0, bytes);
  Text *t = reinterpret_cast<Text *>(raw);
  uint8_t *p = raw + sizeof(Text);
  t->next = reinterpret_cast<uint32_t *>(p);
  p += n * sizeof(uint32_t);
  t->shadow = reinterpret_cast<uint32_t *>(p);
  p += n * sizeof(uint32_t);
  t->cells = reinterpret_cast<uint16_t *>(p);

  t->cols = uint8_t(cols);
  t->rows = uint8_t(rows);
  t->scale = uint8_t(scale);
  for (size_t k = 0; k < n; ++k)
    t->cells[k] = cell(' ', kDefaultAttr);
  t->cursor = make_cursor(0, 0, kCursorDefaultStart, kCursorDefaultEnd, true);
  t->flags = kFlagBlink;
  memcpy(t->palette, kPalette, sizeof kPalette);
  t->seen_flags = t->flags;
  t->full = true;
  return t;
}

void release(Text *t) { free(t); }

bool prepare(Text &t, uint64_t now_ms) {
  const uint32_t flags = ld(t.flags), cur = ld(t.cursor),
                 gen = ld(t.palette_gen);
  if (gen != t.seen_gen || flags != t.seen_flags) {
    t.full = true;
    t.seen_gen = gen;
    t.seen_flags = flags;
  }
  const bool blink = flags & kFlagBlink;
  const bool hide_blinkers = blink && ((now_ms / kBlinkHalfMs) & 1);
  const bool cursor_phase = ((now_ms / kCursorHalfMs) & 1) == 0;
  const uint32_t ccol = cur & 0xFF, crow = (cur >> 8) & 0xFF;
  const bool cursor_on =
      ((cur >> 24) & 1) && cursor_phase && ccol < t.cols && crow < t.rows;
  const uint32_t cidx = crow * t.cols + ccol;
  const uint32_t cbits =
      1u << 16 | ((cur >> 16) & 15) << 20 | ((cur >> 20) & 15) << 24;
  const uint32_t n = uint32_t(t.cols) * t.rows;
  bool any = t.full;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t k = ld(t.cells[i]);
    if (hide_blinkers && (k & 0x8000))
      k |= 0x20000;
    if (cursor_on && i == cidx)
      k |= cbits;
    t.next[i] = k;
    any |= k != t.shadow[i];
  }
  t.pending = any;
  return any;
}

void paint(const Canvas &c, const Text &t) {
  const int s = t.scale, cw = W * s, ch = H * s;
  const int x0 = std::max<int>(c.l, 0), y0 = std::max<int>(c.t, 0);
  const int x1 = std::min<int>(c.r, int(t.cols) * cw);
  const int y1 = std::min<int>(c.b, int(t.rows) * ch);
  if (x0 >= x1 || y0 >= y1)
    return;
  for (int row = y0 / ch; row <= (y1 - 1) / ch; ++row)
    for (int col = x0 / cw; col <= (x1 - 1) / cw; ++col) {
      const uint32_t i = uint32_t(row) * t.cols + uint32_t(col);
      const uint32_t k = t.next[i];
      if (!t.full && k == t.shadow[i])
        continue;
      paint_cell(c, t, col * cw, row * ch, k, x0, y0, x1, y1);
    }
}

void commit(Text &t) {
  memcpy(t.shadow, t.next, sizeof(uint32_t) * t.cols * t.rows);
  t.full = false;
  t.pending = false;
}

// ---- VgaWindow
// ------------------------------------------------------------------
bool VgaWindow::open(const char *title, uint32_t cols, uint32_t rows,
                     uint32_t scale) {
  using namespace WindowManager;
  close();
  tid_ = Kernel::Scheduler::current_thread_id();
  if (register_vga_window(tid_, title, cols, rows, scale) < 0)
    return false;
  t_ = vga_text(tid_);
  return t_ != nullptr;
}

void VgaWindow::close() {
  if (t_)
    WindowManager::unregister_window(tid_);
  t_ = nullptr;
}

bool VgaWindow::has_focus() const {
  return t_ && WindowManager::has_keyboard_focus(tid_);
}

void VgaWindow::put(uint32_t col, uint32_t row, uint8_t ch, uint8_t a) {
  if (t_ && col < t_->cols && row < t_->rows)
    stv(t_->cells[row * t_->cols + col], cell(ch, a));
}

void VgaWindow::print(uint32_t col, uint32_t row, const char *s, uint8_t a) {
  for (; t_ && *s && col < t_->cols; ++s, ++col)
    put(col, row, uint8_t(*s), a);
}

void VgaWindow::clear(uint8_t a) {
  if (!t_)
    return;
  for (uint32_t i = 0; i < uint32_t(t_->cols) * t_->rows; ++i)
    stv(t_->cells[i], cell(' ', a));
}

void VgaWindow::scroll_up(uint32_t lines, uint8_t fill_attr) {
  if (!t_ || !lines)
    return;
  lines = std::min<uint32_t>(lines, t_->rows);
  const uint32_t keep = (t_->rows - lines) * t_->cols;
  memmove(t_->cells, t_->cells + lines * t_->cols, keep * sizeof(uint16_t));
  for (uint32_t i = keep; i < uint32_t(t_->rows) * t_->cols; ++i)
    stv(t_->cells[i], cell(' ', fill_attr));
}

void VgaWindow::set_cursor(uint32_t col, uint32_t row) {
  if (!t_)
    return;
  const uint32_t c = ld(t_->cursor);
  stv(t_->cursor,
      make_cursor(col, row, (c >> 16) & 15, (c >> 20) & 15, (c >> 24) & 1));
}

void VgaWindow::show_cursor(bool v) {
  if (t_)
    stv(t_->cursor, (ld(t_->cursor) & ~(1u << 24)) | uint32_t(v) << 24);
}

void VgaWindow::set_cursor_shape(uint8_t s, uint8_t e) {
  if (!t_)
    return;
  const uint32_t c = ld(t_->cursor);
  stv(t_->cursor, make_cursor(c & 0xFF, (c >> 8) & 0xFF, s, e, (c >> 24) & 1));
}

void VgaWindow::set_blink(bool on) {
  if (t_)
    stv(t_->flags,
        on ? (ld(t_->flags) | kFlagBlink) : (ld(t_->flags) & ~kFlagBlink));
}

void VgaWindow::set_palette(uint8_t i, uint32_t rgb) {
  if (!t_)
    return;
  stv(t_->palette[i & 15], 0xFF000000u | (rgb & 0xFFFFFFu));
  stv(t_->palette_gen, ld(t_->palette_gen) + 1);
}

void VgaWindow::reset_palette() {
  if (!t_)
    return;
  for (int i = 0; i < 16; ++i)
    stv(t_->palette[i], kPalette[i]);
  stv(t_->palette_gen, ld(t_->palette_gen) + 1);
}

} // namespace Kernel::Graphics::Vga
