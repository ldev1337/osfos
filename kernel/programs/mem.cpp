#include <cstdint>
#include <kernel/drivers/keyboard.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/appcommon.h>
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

void draw_formatted_size(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x,
                         int32_t y, size_t bytes, uint32_t color) {
  char buf[32];
  size_t kib = bytes / 1024;
  size_t mib = kib / 1024;

  if (mib > 0) {
    size_t dec = (kib % 1024) * 10 / 1024;
    size_t idx = 0;

    size_t temp = mib;
    char temp_buf[16];
    size_t t_idx = 0;
    do {
      temp_buf[t_idx++] = '0' + (temp % 10);
      temp /= 10;
    } while (temp > 0);
    while (t_idx > 0)
      buf[idx++] = temp_buf[--t_idx];

    buf[idx++] = '.';
    buf[idx++] = '0' + static_cast<char>(dec);
    buf[idx++] = ' ';
    buf[idx++] = 'M';
    buf[idx++] = 'i';
    buf[idx++] = 'B';
    buf[idx] = '\0';
  } else {
    size_t idx = 0;
    size_t temp = kib;
    char temp_buf[16];
    size_t t_idx = 0;
    do {
      temp_buf[t_idx++] = '0' + (temp % 10);
      temp /= 10;
    } while (temp > 0);
    while (t_idx > 0)
      buf[idx++] = temp_buf[--t_idx];

    buf[idx++] = ' ';
    buf[idx++] = 'K';
    buf[idx++] = 'i';
    buf[idx++] = 'B';
    buf[idx] = '\0';
  }

  text(vfb, x, y, buf, color);
}

void draw_stat_card(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x,
                    int32_t y, int32_t w, int32_t h, const char *title,
                    size_t used_bytes, size_t total_bytes, uint32_t pct,
                    uint32_t color) {
  vfb.fill_rounded_rect(x, y, w, h, 6, CARD_BG);
  vfb.draw_rounded_rect(x, y, w, h, 6, CARD_BORDER);

  text(vfb, x + 12, y + 10, title, TEXT_MUTED);
  draw_formatted_size(vfb, x + 12, y + 26, used_bytes, color);

  text(vfb, x + 12, y + 42, "/", TEXT_MUTED);
  draw_formatted_size(vfb, x + 24, y + 42, total_bytes, TEXT_MUTED);

  // Dynamic Progress Bar
  int32_t bar_x = x + 12;
  int32_t bar_y = y + h - 16;
  int32_t bar_w = w - 24;
  int32_t bar_h = 8;
  vfb.fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 3, 0x121A22);

  int32_t fill_w =
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

  while (running && !Kernel::Scheduler::should_stop()) {
    while (Keyboard::available()) {
      char c = Keyboard::getchar();
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
      if (c == 'q' || c == 27) {
        running = false;
        break;
      }
    }
    if (!running)
      break;

    // Fetch System Memory Analytics directly from Heap/PMM subsystems
    Kernel::Memory::SystemMemoryInfo mem_info =
        Kernel::Memory::g_kheap.get_system_memory_info();

    uint32_t pmm_pct =
        (mem_info.pmm_total_memory > 0)
            ? static_cast<uint32_t>((mem_info.pmm_used_memory * 100) /
                                    mem_info.pmm_total_memory)
            : 0;
    uint32_t heap_pct =
        (mem_info.heap_total_size > 0)
            ? static_cast<uint32_t>((mem_info.heap_used_size * 100) /
                                    mem_info.heap_total_size)
            : 0;

    // Sample history every 2 ticks
    if (sample_tick % 2 == 0) {
      history.push(pmm_pct, heap_pct);
    }

    vfb.clear(BG_COLOR);

    // Header Title
    text(vfb, 16, 12, "SYSTEM MEMORY ANALYTICS LIVE", TEXT_MAIN);
    text(vfb, w - 100, 12, "[Q] QUIT", TEXT_MUTED);

    // Status Cards
    int32_t card_w = (w - 44) / 2;
    draw_stat_card(vfb, 16, 32, card_w, 76, "PMM PHYSICAL MEMORY",
                   mem_info.pmm_used_memory, mem_info.pmm_total_memory, pmm_pct,
                   COLOR_PMM);

    draw_stat_card(vfb, 28 + card_w, 32, card_w, 76, "KERNEL HEAP MEMORY",
                   mem_info.heap_used_size, mem_info.heap_total_size, heap_pct,
                   COLOR_HEAP);

    // Heap Peak Memory Panel
    vfb.fill_rounded_rect(16, 116, w - 32, 28, 4, CARD_BG);
    text(vfb, 24, 124, "HEAP PEAK USED:", TEXT_MUTED);
    draw_formatted_size(vfb, 150, 124, mem_info.heap_peak_size, COLOR_PEAK);

    // Real-Time Analytics Graph Area
    int32_t gx = 16;
    int32_t gy = 152;
    int32_t gw = w - 32;
    int32_t gh = h - 168;

    vfb.fill_rounded_rect(gx, gy, gw, gh, 6, CARD_BG);
    vfb.draw_rounded_rect(gx, gy, gw, gh, 6, CARD_BORDER);

    // Axis Grid Lines (25%, 50%, 75%)
    for (int i = 1; i <= 3; ++i) {
      int32_t line_y = gy + gh - (gh * i) / 4;
      vfb.draw_line(gx + 4, line_y, gx + gw - 5, line_y, COLOR_GRID);
    }

    // Plot Usage Curves
    if (history.count > 1) {
      int32_t inner_x = gx + 8;
      int32_t inner_y = gy + 8;
      int32_t inner_w = gw - 16;
      int32_t inner_h = gh - 16;

      for (size_t i = 0; i + 1 < history.count; ++i) {
        MemorySample s1 = history.get(i);
        MemorySample s2 = history.get(i + 1);

        int32_t x1 = inner_x + static_cast<int32_t>((i * inner_w) /
                                                    (HISTORY_CAPACITY - 1));
        int32_t x2 = inner_x + static_cast<int32_t>(((i + 1) * inner_w) /
                                                    (HISTORY_CAPACITY - 1));

        // PMM Line
        int32_t pmm_y1 = inner_y + inner_h -
                         static_cast<int32_t>((s1.pmm_pct * inner_h) / 100);
        int32_t pmm_y2 = inner_y + inner_h -
                         static_cast<int32_t>((s2.pmm_pct * inner_h) / 100);
        vfb.draw_line(x1, pmm_y1, x2, pmm_y2, COLOR_PMM);

        // Heap Line
        int32_t heap_y1 = inner_y + inner_h -
                          static_cast<int32_t>((s1.heap_pct * inner_h) / 100);
        int32_t heap_y2 = inner_y + inner_h -
                          static_cast<int32_t>((s2.heap_pct * inner_h) / 100);
        vfb.draw_line(x1, heap_y1, x2, heap_y2, COLOR_HEAP);
      }
    }

    // Graph Legend
    vfb.fill_rect(gx + 12, gy + gh - 18, 10, 8, COLOR_PMM);
    text(vfb, gx + 26, gy + gh - 18, "PMM", COLOR_PMM);

    vfb.fill_rect(gx + 70, gy + gh - 18, 10, 8, COLOR_HEAP);
    text(vfb, gx + 84, gy + gh - 18, "HEAP", COLOR_HEAP);

    vfb.flush();
    sample_tick++;
    delay(2);
  }
}
} // namespace

void memory_analyzer_run() { run_memory_analyzer(); }
} // namespace Kernel::Apps