 Here is a comprehensive analysis of optimization
  opportunities across kernel/graphics/lwm.cpp (the
  compositor) and kernel/graphics/vfb.cpp (the rasterizer
  and command encoder), taking into account the
  architecture, data structures, and compiler target (-  
  march=x86-64-v3).
  ──────

### Executive Overview
  
  The window management and graphics pipeline in OSFOS
  operates on a split model:

  1. Producer (kernel/graphics/vfb.cpp): Encodes draw
  commands into a lock-free SPSC ring buffer
  (include/kernel/graphics/lwm_draw.h).
  2. Consumer (kernel/graphics/lwm.cpp): Dispatches and  
  rasterizes commands directly to the hardware
  framebuffer clipped to visible window shapes, applying
  window shadows, rounded corners, and tile-based
  composition.
  
  The most impactful optimization opportunities fall into
  five major categories:
  
  1. Memory & Cache Locality (Massive BSS footprint &
  cache thrashing)
  2. Compositor Redundancy & Clip Culling
  (kernel/graphics/lwm.cpp)
  3. Rasterizer Algorithm & Math Improvements
  (kernel/graphics/vfb.cpp)
  4. SIMD Vectorization (-march=x86-64-v3 AVX2/SSE4.2)
  5. IPC Ring Buffer & Batching Overhead
  (include/kernel/graphics/lwm_draw.h)
  ──────

### 1. Memory Architecture & Cache Locality
  
#### 1.1 Decouple Ring from struct Window to Fix 4 MB  

  Cache Thrashing

  • Location: kernel/graphics/lwm.cpp:160-180,
  include/kernel/graphics/lwm_draw.h:37-42
  • Problem: struct Window directly embeds Cmd::Ring ring,
  which is 128 KiB + 192 bytes. The global array Window  
  windows[MAX_WINDOWS] (32 windows) occupies over 4.1 MB
  of BSS memory.
  • Routines like sync_table(), find_live(), publish(),  
  and mouse hit-testing frequently iterate over
  windows[0..31]. Every array step jumps by >131 KiB,
  instantly evicting L1 (32–48 KiB) and L2 (512 KiB–1
  MiB) caches on every access.
  • Optimization: Separate window metadata from ring
  buffers:
    struct WindowMeta { ... }; // ~128 bytes; all 32 fit
    in a single 4 KiB page!
    WindowMeta windows[MAX_WINDOWS];
    Cmd::Ring rings[MAX_WINDOWS]; // or allocated
    dynamically
  Iterating over all 32 windows will then stay entirely  
  within L1 cache.
  
#### 1.2 Cache Static Clip Rectangles (build_clips)
  
  • Location: kernel/graphics/lwm.cpp:958-988,
  kernel/graphics/lwm.cpp:1066-1067
  • Problem: In drain_all(), build_clips(z, open, fringe)
  is invoked every tick for every window with queued
  commands. build_clips performs O(W²) polygon/rectangle
  subtractions and re-evaluates corner radii tables.
  • Window positions and z-orders only change during
  dragging, resizing, or focus changes. During normal
  application rendering (Doom, terminal, GUI redraws),
  geometry is 100% static.
  • Optimization: Cache the calculated open and fringe
  clip rect lists per window. Tag windows with a
  clip_generation counter; invalidate and recalculate
  clip rects only when window bounds or stacking change.
  ──────

### 2. Compositor Pipeline & Redundancy Reduction

  (kernel/graphics/lwm.cpp)

#### 2.1 Bounding-Box Culling of Ring Commands against

  Clip Rectangles

  • Location: kernel/graphics/lwm.cpp:1080-1084
  • Problem: In drain_all(), every record is executed
  against all open.n clip rects:
    for_records(q, t, h, [&](const uint32_t *rec) {
      for (size_t i = 0; i < n; ++i)
        Raster::execute(cv[i], rec);
    });
  Because of bottom rounded corners, open.n typically
  contains 5–10 clip rectangles. If an app draws a button
  at the top of the window, Raster::execute is still
  called for the 4–8 clip slices at the bottom of the
  window.
  • Optimization: Compute the command's bounding box once
  before dispatching across cv[i]. If the command does
  not overlap cv[i], skip it immediately. Furthermore, if
  the command is completely contained within the main
  body rectangle, skip all other clip rects.
  
#### 2.2 Hoist Invariants in Shadow Shading (shade_flat

  & shade_generic)

  • Location: kernel/graphics/lwm.cpp:360-393
  • Problem:
      • SHADOW is a compile-time constant (0xFF020609).  
      • In shade_flat: al = (rowA + 128u) >> 8 is
      constant across the entire span [a, b).
      • Yet inside mix(row[px], SHADOW, al), it
      recalculates (SHADOW & 0x00FF00FFu) *al and
      (SHADOW & 0x0000FF00u)* al for every single pixel!
  • Optimization: Precalculate s_rb = (SHADOW &
  0x00FF00FFu) *al and s_g = (SHADOW & 0x0000FF00u)* al,
  along with ia = 256 - al, once per span. Shading a flat
  span then reduces to a single multiply-add per pixel
  (or 8 pixels at once via AVX2).
  
#### 2.3 Skip Obscured Background Fills in compose_tile

  • Location: kernel/graphics/lwm.cpp:754-770
  • Problem: compose_tile fills scratch with DESKTOP,
  then paints shadows, titles, and bodies of all
  overlapping windows bottom-up. If a top-level window
  completely covers the tile T, the desktop prefill and  
  lower-window shadow rendering are completely
  overwritten.
  • Optimization: Inspect the topmost window covering T:
  if its opaque body covers T, skip the DESKTOP fill and
  skip rendering windows occluded beneath it.
  
#### 2.4 Vectorize Mask Comparisons in run_fringe_tile
  
  • Location: kernel/graphics/lwm.cpp:1006-1017
  • Problem: run_fringe_tile compares s[x] != f[x] pixel-
  by-pixel in a scalar loop to construct the changed mask.
  • Optimization: Compare 8 dwords at once using
  _mm256_cmpeq_epi32 +_mm256_movemask_epi8. Additionally,
  avoid invoking memcpy for short 1- to 4-pixel changed  
  runs; use direct register stores.
  
#### 2.5 Avoid Unconditional Atomic Stores in publish()
  
  • Location: kernel/graphics/lwm.cpp:587-597
  • Problem: publish() performs atomic stores to pos_x
  and pos_y for all windows on every single compositor
  tick, creating unnecessary cache line invalidations
  across cores even when windows are idle.
  • Optimization: Only write pos_x and pos_y when
  position values actually differ from the previously
  published values.
  ──────

### 3. Rasterization & Math Optimizations

  (kernel/graphics/vfb.cpp)
  
#### 3.1 Hardware Double-Precision Square Root in isqrt
  
  • Location: kernel/graphics/vfb.cpp:156-169
  • Problem: isqrt(int64_t v) is used per scanline in
  disc(), ring(), and sector(). It currently executes a  
  15–30 iteration digit-recurrence bitwise loop.
  • Optimization: On x86-64 with -march=x86-64-v3,
  hardware double-precision square root (sqrtsd
  instruction via_mm_sqrt_sd or __builtin_sqrt) computes
  in ~12–14 cycles with zero branch divergence. Since v ≤
  R² (a few million for window elements), v fits
  losslessly in the 53-bit mantissa of an IEEE-754
  double:
    inline int64_t isqrt(int64_t v) {
      return v < 2 ? v :
      static_cast<int64_t>(__builtin_sqrt(static_cast<dou
      ble>(v)));
    }
  
#### 3.2 Segment Decomposition in cover_row

  • Location: kernel/graphics/vfb.cpp:223-234
  • Problem: Inside cover_row:
    for (int64_t x = a; x <= z; ++x) {
      if (x >= fa && x <= fz) { ... }
      else if (x >= ha && x <= hz) { ... }
      else { put_cov(...); }
    }
  Every pixel tests interval ranges and jumps.
  • Optimization: The intervals [a, min(fa - 1, z)], [fa,
  fz], and [max(fz + 1, a), z] are static for the
  scanline. Decompose the row into 3 direct phases:
      1. Left antialiased edge: x from a to min(fa - 1,  
      z) (put_cov)
      2. Solid interior: span_fill(row + fa, fz - fa + 1,
      color, alpha)
      3. Right antialiased edge: x from max(fz + 1, a) to
      z (put_cov)
      This completely removes loop-carried branches.
  
#### 3.3 Hoist Constants in span_fill
  
  • Location: kernel/graphics/vfb.cpp:90-95
  • Problem: span_fill calls mix(color, p[i], alpha) for
  every pixel. color and alpha are invariant across all n
  pixels.
  • Optimization:
      1. Add early exit: if (alpha == 0) return;
      2. Precompute fg_rb = (color & 0x00FF00FFu) *alpha
      + 0x00800080u and fg_g = ((color >> 8) & 255)*
      alpha + 128 outside the loop.
  
#### 3.4 Invert Mode Branch in blit_any
  
  • Location: kernel/graphics/vfb.cpp:514-527
  • Problem: blit_any checks if (mode == BlitOpaque) ...
  else if (mode == BlitKey) ... inside the per-scanline y
  loop.
  • Optimization: Move switch(mode) outside the y loop
  into three specialized scanline routines:
      • BlitOpaque: straightforward row memcpy
      • BlitKey: vectorized comparison without branch
      penalties
      • BlitAlpha: hoisted alpha multiplier
  
#### 3.5 Adaptive Angular Step in walk_arc
  
  • Location: kernel/graphics/vfb.cpp:455-467
  • Problem: walk_arc iterates degree-by-degree (deg =
  start; deg <= end; ++deg), drawing line segments
  between adjacent degrees. For small radii, adjacent
  degrees map to identical or 1-pixel coordinates,
  invoking Bresenham line setup up to 360 times for a 20-
  pixel arc.
  • Optimization: Step by Δθ ≈ max(1, 45/r) or use a
  midpoint/Bresenham arc generator.
  ──────

### 4. SIMD & Vectorization Opportunities (-march=x86-

  64-v3)
  
  The build configuration specifies -march=x86-64-v3,
  meaning AVX2, SSE4.2, BMI2, and FMA are available
  across the entire kernel:
  
#### 4.1 AVX2 Alpha Blending Vectorization
  
  • Applications: span_fill, fill_rect_alpha, blit_alpha,
  shade_flat, paint_shadow.
  • Implementation: Unpack 8 pixels from 8-bit to 16-bit
  integers (_mm256_unpacklo_epi8,_mm256_unpackhi_epi8),
  multiply with _mm256_mullo_epi16, add bias, shift, and
  repack with_mm256_packus_epi16.
  • Speedup: 4x–8x throughput increase on alpha-heavy
  rendering.

#### 4.2 Replace rep stosl in fill32 with AVX2 Unrolled

  Stores

  • Location: kernel/graphics/vfb.cpp:60-67
  • Problem: rep stosl has high startup latency (~20–25  
  cycles) on modern Intel/AMD CPUs. For typical GUI
  scanline fills (30–300 pixels), unrolled
  _mm256_storeu_si256 (8 pixels per store) significantly
  outperforms rep stosl. For large screen fills (>2048
  pixels), pairing 32-bit values into 64-bit qwords with
  rep stosq doubles memory throughput.
  ──────

### 5. IPC Ring Buffer & Command Protocol

  (include/kernel/graphics/lwm_draw.h)

#### 5.1 Producer-Side Cached Ring Tail

  • Location: include/kernel/graphics/lwm_draw.h:45-61
  • Problem: Every single primitive submission calls
  Cmd::begin(q, n), which executes __atomic_load_n(&q-
  >tail, __ATOMIC_ACQUIRE).
  • Optimization: Add uint32_t cached_tail to the
  producer state. Only execute the atomic acquire reload
  when kRingWords - (h - cached_tail) < adv.

#### 5.2 Batching for High-Frequency Primitives

  (put_pixel, draw_line)

  • Location: kernel/graphics/vfb.cpp:931-935
  • Problem: put_pixel encodes 3 words (12 bytes) per
  individual pixel. Submitting 1,000 pixels consumes ~10%
  of the ring buffer and forces 1,000 separate
  Raster::execute dispatch iterations in the compositor.
  • Optimization: Introduce batched commands such as
  OpPixelSpan (horizontal span) or OpPointList (array of

  points sharing a color).
  ──────

### Prioritized Recommendation Matrix
  
   Prio… │ Optimization │ Target File │ Expected Benefit
  ───────┼──────────────┼─────────────┼──────────────────
   P0    │ Decouple     │ kernel/grap │ Eliminates 4 MB
         │ Ring from    │ hics/lwm.cp │ BSS cache
         │ Window       │ p,          │ footprint; all
         │              │ include/ker │ window lookups
         │              │ nel/graphic │ stay in L1 cache
         │              │ s/lwm_draw. │
   P0    │              │ h           │
   P0    │ Cache Static │ kernel/grap │ Eliminates O(W²)
         │ Clip         │ hics/lwm.cp │ polygon
         │ Rectangles   │ p           │ subtractions and
         │              │             │ corner math on
   P1    │              │             │ static frames
   P1    │ Bounding-Box │ kernel/grap │ Avoids 60–80% of
         │ Culling in   │ hics/lwm.cp │ redundant
         │ drain_all    │ p,          │ Raster::execute
         │              │ kernel/grap │ calls across
         │              │ hics/vfb.cp │ corner clip
         │              │ p           │ rects
   P1    │ Hardware     │ kernel/grap │ Replaces 30-
         │ sqrtsd for   │ hics/vfb.cp │ iteration bit
         │ isqrt        │ p           │ loop with ~12-
         │              │             │ cycle
         │              │             │ instruction in
         │              │             │ circle/disc/sect
         │              │             │ or
   P1    │ Decompose    │ kernel/grap │ Eliminates per-
         │ cover_row    │ hics/vfb.cp │ pixel branching
         │ Scanlines    │ p           │ and jumps in
         │              │             │ antialiased
         │              │             │ shape drawing
   P2    │ AVX2 Alpha   │ kernel/grap │ 4x–8x throughput
         │ Blending     │ hics/vfb.cp │ speedup on alpha
         │ Vectorizatio │ p,          │ fills, blits,
         │ n            │ kernel/grap │ and shadow
         │              │ hics/lwm.cp │ shading
         │              │ p           │
   P2    │ Hoist Shadow │ kernel/grap │ Saves
         │ & Alpha      │ hics/lwm.cp │ multiplications
         │ Invariants   │ p,          │ and bit shifts
         │              │ kernel/grap │ per pixel across
         │              │ hics/vfb.cp │ spans
         │              │ p           │
