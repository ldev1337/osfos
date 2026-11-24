// vga_text.h - emulated VGA text-mode window (80x25 style, CP437, 16 colours).
//
// A VGA window is an ordinary compositor window (same chrome, drag, focus,
// z-order, shadows, close button, keyboard focus, position query) whose client
// area is not fed by a command ring but by a hardware-style cell buffer, the
// way a real text-mode adapter works:
//
//   cells[row * cols + col] = ch | attr << 8      // like 0xB8000
//
// Applications simply write cells. The compositor diffs them against what it
// last painted and repaints only the cells that changed, honouring blink,
// cursor shape and the 16-entry palette. Nothing has to be "flushed" and
// exposure / repaint after being uncovered is handled by the compositor.
//
// Glyphs: one 9x16 coverage atlas built once from the system TrueType font
// (size auto-fitted so the advance is exactly 9 px, baseline pinned to the
// cell grid). CP437 line / block / shade characters (0xB0..0xDF) are generated
// procedurally so they always tile seamlessly, like on real hardware.
#pragma once

#include <cstdint>

#include <kernel/graphics/internal.h> // Canvas

namespace Kernel::Graphics::Vga {

constexpr uint32_t kCellW = 9, kCellH = 16; // classic VGA 9x16 cell
constexpr uint32_t kMaxCols = 80, kMaxRows = 50, kMaxCells = kMaxCols * kMaxRows;
constexpr uint32_t kMaxScale = 4; // integer pixel scale, 1..4

// ---- cell / attribute helpers ------------------------------------------------
constexpr uint8_t attr(uint8_t fg, uint8_t bg) {
  return uint8_t((fg & 15) | (bg & 15) << 4);
}
constexpr uint16_t cell(uint8_t ch, uint8_t a) {
  return uint16_t(ch | uint16_t(a) << 8);
}
constexpr uint8_t kDefaultAttr = 0x07; // light grey on black

// flags
enum : uint32_t {
  kFlagBlink = 1u, // attr bit 7 blinks (default, like BIOS); clear => bright bg
};

// cursor register: col | row << 8 | start << 16 | end << 20 | visible << 24
constexpr uint32_t make_cursor(uint32_t col, uint32_t row, uint32_t start,
                               uint32_t end, bool visible) {
  return (col & 0xFF) | (row & 0xFF) << 8 | (start & 15) << 16 |
         (end & 15) << 20 | uint32_t(visible) << 24;
}

// One emulated adapter, heap-allocated by alloc() as a single malloc block:
// this header followed by next[], shadow[] and cells[] sized to cols*rows
// (about 10 bytes per cell: 20 KB for 80x25). The app-side fields behave like
// memory-mapped registers (plain loads/stores, no locking, tearing between two
// frames is possible exactly as on hardware).
struct Text {
  // ---- app side ----
  uint16_t *cells; // cols*rows entries, row-major (points into this block)
  uint32_t cursor;
  uint32_t flags;
  uint32_t palette[16]; // 0xFFRRGGBB
  uint32_t palette_gen; // bump after changing palette[]
  uint8_t cols, rows, scale;

  // ---- compositor side (never touched by apps) ----
  bool pending; // prepare() found work, cleared by commit()
  bool full;    // repaint every cell
  uint32_t seen_gen, seen_flags;
  uint32_t *shadow; // key last painted (cols*rows)
  uint32_t *next;   // key to paint this frame (cols*rows)
};

// Compositor-thread entry points ------------------------------------------------
void init_atlas();                      // once, at compositor init
Text *alloc(uint32_t cols, uint32_t rows, uint32_t scale); // malloc; nullptr on OOM
void release(Text *t);                                      // free
bool prepare(Text &t, uint64_t now_ms); // diff + blink; true if work pending
void paint(const Canvas &c, const Text &t); // paint pending cells clipped to c
void commit(Text &t);                   // frame done: shadow = next
inline void invalidate(Text &t) { t.full = true; }

// App-side convenience wrapper -----------------------------------------------------
class VgaWindow {
  Text *t_ = nullptr;
  uint64_t tid_ = 0;

public:
  // Registers a window for the calling thread. 720x400 client at 80x25, scale 1.
  bool open(const char *title, uint32_t cols = 80, uint32_t rows = 25,
            uint32_t scale = 1);
  void close();
  bool valid() const { return t_ != nullptr; }
  bool has_focus() const;

  uint32_t cols() const { return t_ ? t_->cols : 0; }
  uint32_t rows() const { return t_ ? t_->rows : 0; }
  uint32_t width_px() const { return t_ ? t_->cols * kCellW * t_->scale : 0; }
  uint32_t height_px() const { return t_ ? t_->rows * kCellH * t_->scale : 0; }

  // Raw "video memory" (rows * cols entries, row-major).
  uint16_t *cells() { return t_ ? t_->cells : nullptr; }

  void put(uint32_t col, uint32_t row, uint8_t ch, uint8_t a = kDefaultAttr);
  void print(uint32_t col, uint32_t row, const char *s,
             uint8_t a = kDefaultAttr); // clipped to the row, no wrap
  void clear(uint8_t a = kDefaultAttr);
  void scroll_up(uint32_t lines = 1, uint8_t fill_attr = kDefaultAttr);

  void set_cursor(uint32_t col, uint32_t row);
  void show_cursor(bool visible);
  void set_cursor_shape(uint8_t start_line, uint8_t end_line); // 0..15
  void set_blink(bool attr_blinks); // false => attr bit 7 = bright background
  void set_palette(uint8_t index, uint32_t rgb);
  void reset_palette();
};

} // namespace Kernel::Graphics::Vga
