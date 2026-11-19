// kernel/graphics/lwm_draw.h
//
// Glue between lwm.cpp (compositor) and vfb.cpp (rasteriser + encoder).
//
// Model: every window owns a single-producer / single-consumer command ring.
// The application thread (producer) only *encodes* a few words per primitive.
// The compositor thread (consumer) is the only writer of window pixels: it
// decodes the ring and rasterises into the hardware framebuffer, clipped to
// the visible part of the window. No locks, no waiting, no sleeping on the
// application side; a full ring drops the command.
#pragma once

#include <cstddef>
#include <cstdint>

#include <kernel/graphics/truetype.h>

namespace Kernel::Graphics {

// Drawing target. Local pixel (x, y) lives at origin[y * stride + x];
// clip is half-open, local coordinates.
struct Canvas {
  uint32_t *origin;
  ptrdiff_t stride;
  int32_t l, t, r, b;
};

namespace Cmd {

// Record = 1 header word (op | len << 8, len in words incl. header) + payload.
// Records never wrap: the producer pads to the end of the ring instead.
constexpr uint32_t kRingWords = 1u << 15; // 128 KiB per window
constexpr uint32_t kMask = kRingWords - 1;
constexpr uint32_t kPad = 0;
constexpr uint32_t kInlinePixels = 2048; // blits up to this size are copied

struct Ring {
  alignas(64) uint32_t head; // producer-owned
  alignas(64) uint32_t tail; // consumer-owned
  uint32_t pend;             // producer scratch
  uint32_t cached_tail;      // producer scratch
  alignas(64) uint32_t buf[kRingWords];
};

// Reserves n words (header included). nullptr if the ring is full: drop.
inline uint32_t *begin(Ring *q, uint32_t n) {
  if (n > kRingWords / 2) [[unlikely]]
    return nullptr;
  const uint32_t h = q->head;
  uint32_t t = q->cached_tail;
  uint32_t idx = h & kMask;
  const uint32_t room = kRingWords - idx;
  const uint32_t adv = n > room ? room + n : n;
  if (kRingWords - (h - t) < adv) {
    t = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    q->cached_tail = t;
    if (kRingWords - (h - t) < adv) [[unlikely]]
      return nullptr;
  }
  if (n > room) {
    q->buf[idx] = kPad | (room << 8);
    idx = 0;
  }
  q->pend = h + adv;
  return q->buf + idx;
}

// Publishes the record written after begin().
inline void end(Ring *q) { __atomic_store_n(&q->head, q->pend, __ATOMIC_RELEASE); }

// Queues a pixel blit. mode: 0 opaque, 1 colour-key (param = key),
// 2 global alpha (param = alpha). Small sources are copied into the ring;
// larger ones are read BY REFERENCE when the compositor executes the record,
// so they must stay valid and unchanged until then (use a persistent buffer).
void blit(Ring *q, int32_t x, int32_t y, uint32_t w, uint32_t h,
          const uint32_t *src, uint32_t pitch, uint32_t mode, uint32_t param);

} // namespace Cmd

namespace Raster { // implemented in vfb.cpp
TrueTypeFont *default_font(); // may be null
Canvas screen();              // whole hardware framebuffer (origin null if none)
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
void execute(const Canvas &c, const uint32_t *record); // decode + draw one record
bool get_record_bounds(const uint32_t *record, int32_t &l, int32_t &t, int32_t &r, int32_t &b);
} // namespace Raster

namespace WindowManager { // implemented in lwm.cpp
// The calling thread's command ring, or nullptr if it has no window.
Cmd::Ring *queue_of(uint64_t thread_id);

// True (once) if part of the window was repainted by the compositor and the
// application must redraw its contents.
bool take_expose(uint64_t thread_id);
} // namespace WindowManager

} // namespace Kernel::Graphics
