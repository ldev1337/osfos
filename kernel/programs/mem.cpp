#include <cstdint>
#include <kernel/drivers/keyboard.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/appcommon.h>
#include <oslib/ui.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>

namespace Kernel::Apps {
namespace {
using namespace Kernel::AppCommon;
using Kernel::Drivers::Keyboard;

constexpr uint32_t BG_COLOR = 0x101820;
constexpr uint32_t CARD_BG = 0x1A2634;
constexpr uint32_t CARD_BORDER = 0x2A3C50;
constexpr uint32_t TEXT_MAIN = 0xF1F5F8;
constexpr uint32_t TEXT_MUTED = 0x8AAFC4;
constexpr uint32_t COLOR_PMM = 0x42A5F5;  // Cyan/Blue for PMM
constexpr uint32_t COLOR_HEAP = 0x66BB6A; // Green for Heap
constexpr uint32_t COLOR_PEAK = 0xF4511E; // Red/Orange for Peak Memory
constexpr uint32_t COLOR_GRID = 0x22313E;
constexpr uint32_t COLOR_CHECK = 0xFFD166;
constexpr uint32_t COLOR_OK = 0x66BB6A;
constexpr uint32_t COLOR_ERR = 0xEF5350;

constexpr size_t HISTORY_CAPACITY = 60;

struct MemorySample {
  uint32_t pmm_pct;  // 0 - 100
  uint32_t heap_pct; // 0 - 100
};

class MemoryHistory {
public:
  MemorySample samples[HISTORY_CAPACITY]{};
  size_t count = 0;
  size_t head = 0;

  void push(uint32_t pmm_pct, uint32_t heap_pct) {
    samples[head] = {pmm_pct, heap_pct};
    head = (head + 1) % HISTORY_CAPACITY;
    if (count < HISTORY_CAPACITY)
      ++count;
  }

  MemorySample get(size_t index) const {
    if (index >= count)
      return {0, 0};
    size_t start = (count < HISTORY_CAPACITY) ? 0 : head;
    return samples[(start + index) % HISTORY_CAPACITY];
  }
};

uint32_t calc_pct(size_t used, size_t total) {
  if (total == 0 || used == 0)
    return 0;
  if (used >= total)
    return 100;
  const uint64_t pct = (static_cast<uint64_t>(used) * 100ULL) / total;
  return static_cast<uint32_t>(pct > 100ULL ? 100ULL : pct);
}

void append_uint(char *buf, size_t &idx, size_t cap, size_t val) {
  if (idx + 1 >= cap)
    return;
  char tmp[24];
  size_t t_idx = 0;
  do {
    tmp[t_idx++] = static_cast<char>('0' + (val % 10));
    val /= 10;
  } while (val > 0 && t_idx < sizeof(tmp));
  while (t_idx > 0 && idx + 1 < cap) {
    buf[idx++] = tmp[--t_idx];
  }
  buf[idx] = '\0';
}

void append_str(char *buf, size_t &idx, size_t cap, const char *s) {
  while (*s && idx + 1 < cap) {
    buf[idx++] = *s++;
  }
  buf[idx] = '\0';
}

void draw_formatted_size(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x,
                         int32_t y, size_t bytes, uint32_t color) {
  char buf[32];
  buf[0] = '\0';
  size_t idx = 0;
  const size_t kib = bytes / 1024;
  const size_t mib = kib / 1024;

  if (mib > 0) {
    const size_t dec = ((kib % 1024) * 10) / 1024;
    append_uint(buf, idx, sizeof(buf), mib);
    if (idx + 1 < sizeof(buf))
      buf[idx++] = '.';
    if (idx + 1 < sizeof(buf))
      buf[idx++] = static_cast<char>('0' + dec);
    append_str(buf, idx, sizeof(buf), " MiB");
  } else {
    append_uint(buf, idx, sizeof(buf), kib);
    append_str(buf, idx, sizeof(buf), " KiB");
  }

  Kernel::UICommon::text(vfb, x, y, buf, color);
}

void draw_stat_card(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x,
                    int32_t y, int32_t w, int32_t h, const char *title,
                    size_t used_bytes, size_t total_bytes, uint32_t pct,
                    uint32_t color) {
  vfb.fill_rounded_rect(x, y, w, h, 6, CARD_BG);
  vfb.draw_rounded_rect(x, y, w, h, 6, CARD_BORDER);

  Kernel::UICommon::text(vfb, x + 12, y + 10, title, TEXT_MUTED);
  draw_formatted_size(vfb, x + 12, y + 26, used_bytes, color);

  Kernel::UICommon::text(vfb, x + 12, y + 42, "/", TEXT_MUTED);
  draw_formatted_size(vfb, x + 24, y + 42, total_bytes, TEXT_MUTED);

  // Dynamic Progress Bar
  const int32_t bar_x = x + 12;
  const int32_t bar_y = y + h - 16;
  const int32_t bar_w = w - 24;
  const int32_t bar_h = 8;
  vfb.fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 3, 0x121A22);

  const int32_t fill_w =
      static_cast<int32_t>((static_cast<uint64_t>(bar_w) * pct) / 100);
  if (fill_w > 0) {
    vfb.fill_rounded_rect(bar_x, bar_y, fill_w, bar_h, 3, color);
  }
}

void run_memory_analyzer() {
  Kernel::Graphics::VirtualFramebuffer vfb;
  if (!vfb.open_window(640, 440, 400, 300))
    return;
  const int32_t w = static_cast<int32_t>(vfb.width());
  const int32_t h = static_cast<int32_t>(vfb.height());

  MemoryHistory history;
  bool running = true;
  uint32_t sample_tick = 0;
  bool has_verified_sample = false;
  bool has_heap_check = false;
  bool heap_valid = false;
  Kernel::Memory::SystemMemoryInfo verified_info{};

  while (running && !Kernel::Scheduler::should_stop()) {
    while (Keyboard::available()) {
      char c = Keyboard::getchar();
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
      if (c == 'q' || c == 27) {
        running = false;
        break;
      }
      if (c == 'r') {
        verified_info = Kernel::Memory::g_kheap.get_system_memory_info(true);
        has_verified_sample = true;
      }
      if (c == 'v') {
        heap_valid = Kernel::Memory::g_kheap.validate();
        has_heap_check = true;
      }
    }
    if (!running)
      break;

    // Fetch live System Memory Analytics directly from Heap/PMM subsystems
    const Kernel::Memory::SystemMemoryInfo mem_info =
        Kernel::Memory::g_kheap.get_system_memory_info(false);

    const uint32_t pmm_pct =
        calc_pct(mem_info.pmm_used_memory, mem_info.pmm_total_memory);
    const uint32_t heap_pct =
        calc_pct(mem_info.heap_used_size, mem_info.heap_total_size);

    // Sample history every 2 ticks
    if (sample_tick % 2 == 0) {
      history.push(pmm_pct, heap_pct);
    }

    vfb.clear(BG_COLOR);

    // Header Title & Controls
    Kernel::UICommon::text(vfb, 16, 12, "SYSTEM MEMORY ANALYTICS LIVE", TEXT_MAIN);
    Kernel::UICommon::text(vfb, w - 210, 12, "[R] VERIFY  [V] CHECK  [Q] QUIT",
                           TEXT_MUTED);

    // Status Cards (reflecting live dynamic heap pool size & PMM usage)
    const int32_t card_w = (w - 44) / 2;
    draw_stat_card(vfb, 16, 32, card_w, 76, "PMM PHYSICAL MEMORY",
                   mem_info.pmm_used_memory, mem_info.pmm_total_memory, pmm_pct,
                   COLOR_PMM);

    draw_stat_card(vfb, 28 + card_w, 32, card_w, 76, "DYNAMIC KERNEL HEAP",
                   mem_info.heap_used_size, mem_info.heap_total_size, heap_pct,
                   COLOR_HEAP);

    // Heap Peak & True Recheck Panel
    vfb.fill_rounded_rect(16, 116, w - 32, 40, 4, CARD_BG);
    Kernel::UICommon::text(vfb, 24, 122, "HEAP PEAK:", TEXT_MUTED);
    draw_formatted_size(vfb, 112, 122, mem_info.heap_peak_size, COLOR_PEAK);

    Kernel::UICommon::text(vfb, 234, 122, "POOL:", TEXT_MUTED);
    draw_formatted_size(vfb, 282, 122, mem_info.heap_total_size, COLOR_HEAP);

    if (has_heap_check) {
      Kernel::UICommon::text(vfb, w - 120, 122,
                             heap_valid ? "HEAP: OK" : "HEAP: CORRUPT",
                             heap_valid ? COLOR_OK : COLOR_ERR);
    }

    if (has_verified_sample) {
      Kernel::UICommon::text(vfb, 24, 140, "TRUE PMM:", TEXT_MUTED);
      draw_formatted_size(vfb, 104, 140, verified_info.pmm_used_memory,
                          COLOR_CHECK);
      Kernel::UICommon::text(vfb, 218, 140, "TRUE HEAP:", TEXT_MUTED);
      draw_formatted_size(vfb, 304, 140, verified_info.heap_used_size,
                          COLOR_CHECK);
      Kernel::UICommon::text(vfb, 420, 140, "FREE RAM:", TEXT_MUTED);
      draw_formatted_size(vfb, 502, 140, verified_info.pmm_free_memory,
                          COLOR_PMM);
    } else {
      Kernel::UICommon::text(
          vfb, 24, 140,
          "PRESS [R] TO SCAN TRUE BITMAP/SEGMENT USAGE OR [V] TO VALIDATE",
          COLOR_CHECK);
    }

    // Real-Time Analytics Graph Area
    const int32_t gx = 16;
    const int32_t gy = 164;
    const int32_t gw = w - 32;
    const int32_t gh = h - 168;

    vfb.fill_rounded_rect(gx, gy, gw, gh, 6, CARD_BG);
    vfb.draw_rounded_rect(gx, gy, gw, gh, 6, CARD_BORDER);

    // Axis Grid Lines (25%, 50%, 75%)
    for (int i = 1; i <= 3; ++i) {
      const int32_t line_y = gy + gh - (gh * i) / 4;
      vfb.draw_line(gx + 4, line_y, gx + gw - 5, line_y, COLOR_GRID);
    }

    // Plot Usage Curves
    if (history.count > 1) {
      const int32_t inner_x = gx + 8;
      const int32_t inner_y = gy + 8;
      const int32_t inner_w = gw - 16;
      const int32_t inner_h = gh - 16;

      for (size_t i = 0; i + 1 < history.count; ++i) {
        const MemorySample s1 = history.get(i);
        const MemorySample s2 = history.get(i + 1);

        const int32_t x1 =
            inner_x +
            static_cast<int32_t>((i * inner_w) / (HISTORY_CAPACITY - 1));
        const int32_t x2 =
            inner_x +
            static_cast<int32_t>(((i + 1) * inner_w) / (HISTORY_CAPACITY - 1));

        // PMM Line
        const int32_t pmm_y1 =
            inner_y + inner_h -
            static_cast<int32_t>((s1.pmm_pct * inner_h) / 100);
        const int32_t pmm_y2 =
            inner_y + inner_h -
            static_cast<int32_t>((s2.pmm_pct * inner_h) / 100);
        vfb.draw_line(x1, pmm_y1, x2, pmm_y2, COLOR_PMM);

        // Heap Line
        const int32_t heap_y1 =
            inner_y + inner_h -
            static_cast<int32_t>((s1.heap_pct * inner_h) / 100);
        const int32_t heap_y2 =
            inner_y + inner_h -
            static_cast<int32_t>((s2.heap_pct * inner_h) / 100);
        vfb.draw_line(x1, heap_y1, x2, heap_y2, COLOR_HEAP);
      }
    }

    // Graph Legend
    vfb.fill_rect(gx + 12, gy + gh - 18, 10, 8, COLOR_PMM);
    Kernel::UICommon::text(vfb, gx + 26, gy + gh - 18, "PMM", COLOR_PMM);

    vfb.fill_rect(gx + 70, gy + gh - 18, 10, 8, COLOR_HEAP);
    Kernel::UICommon::text(vfb, gx + 84, gy + gh - 18, "HEAP", COLOR_HEAP);

    vfb.flush();
    sample_tick++;
    delay(2);
  }
}
} // namespace

void memory_analyzer_run() { run_memory_analyzer(); }
} // namespace Kernel::Apps
