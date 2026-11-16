// kernel/graphics/lwm_draw.h
//
// Shared glue between lwm.cpp (window manager / compositor) and vfb.cpp
// (drawing primitives).  There is NO off-screen pixel memory any more:
// every primitive writes straight into the hardware framebuffer, clipped to
// the part of the window that is currently visible.
#pragma once

#include <cstddef>
#include <cstdint>

#include <kernel/graphics/truetype.h>

namespace Kernel::Graphics {

// A drawing target. Pixel (x, y) in local space lives at
// origin[y * stride + x].  `origin` may point outside the screen (a window
// that is partly off-screen); it is only dereferenced inside the clip.
struct Canvas {
  uint32_t *origin;
  ptrdiff_t stride; // pixels per row
  int32_t l, t, r, b; // clip rectangle, local coordinates, half-open
};

namespace Raster { // implemented in vfb.cpp
TrueTypeFont *default_font(); // shared by apps and the compositor; may be null
uint32_t blend(uint32_t fg, uint32_t bg, uint32_t alpha);
void fill_rect(const Canvas &c, int32_t x, int32_t y, int32_t w, int32_t h,
               uint32_t color);
void fill_rounded_rect(const Canvas &c, int32_t x, int32_t y, int32_t w,
                       int32_t h, int32_t radius, uint32_t color);
void fill_circle(const Canvas &c, int32_t xc, int32_t yc, int32_t r,
                 uint32_t color);
int text(const Canvas &c, TrueTypeFont &font, int32_t x, int32_t y,
         float size, const char *text, uint32_t color);
void glyph8(const Canvas &c, int32_t x, int32_t y, char ch, uint32_t color);
} // namespace Raster

namespace WindowManager { // implemented in lwm.cpp
constexpr size_t kMaxClip = 32;

struct ClipRect {
  int32_t l, t, r, b;
};

// Result of begin_draw(): where local (0,0) is on screen and which screen
// rectangles of it may be written right now.
struct Surface {
  uint32_t *origin;
  ptrdiff_t stride;
  int32_t ox, oy; // screen position of local (0,0)
  uint32_t win_w, win_h;
  uint32_t n;
  ClipRect clip[kMaxClip]; // screen coordinates
};

enum class DrawStatus { Ok, NoWindow, Busy };

// Brackets every burst of pixel writes made by an application. While a burst
// is open the compositor waits; while the compositor works, bursts wait.
// `windowed`: fail with NoWindow instead of falling back to raw screen
// coordinates (fx, fy) when the calling thread has no window.
// (bx, by, bw, bh) is the local bounding box about to be touched (used to
// hide the software cursor).
DrawStatus begin_draw(uint64_t thread_id, bool windowed, int32_t fx,
                      int32_t fy, int32_t bx, int32_t by, int32_t bw,
                      int32_t bh, Surface &out);
void end_draw();

// True (once) if part of the window was repainted by the compositor and the
// application must redraw its contents.
bool take_expose(uint64_t thread_id);
} // namespace WindowManager

} // namespace Kernel::Graphics