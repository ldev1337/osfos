#include <cstddef>
#include <cstdint>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/pit.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/appcommon.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>

namespace Kernel::Apps {
namespace {

constexpr uint32_t COLOR_BACKGROUND = 0xFF18232E;
constexpr uint32_t COLOR_HEADER = 0xFF42698A;
constexpr uint32_t COLOR_TEXT = 0xFFF1F5F8;
constexpr uint32_t COLOR_MUTED = 0xFF8AAFC4;
constexpr size_t MAX_TASKS = Kernel::Scheduler::MAX_APPLICATION_TASKS;

size_t format_number(uint64_t value, char *output, size_t capacity) {
  if (!capacity)
    return 0;

  char reversed[24];
  size_t digits = 0;
  do {
    reversed[digits++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value && digits < sizeof(reversed));

  const size_t written = digits < capacity - 1 ? digits : capacity - 1;
  for (size_t i = 0; i < written; ++i)
    output[i] = reversed[digits - i - 1];
  output[written] = '\0';
  return written;
}

void format_uptime(uint32_t elapsed_ms, char *output, size_t capacity) {
  if (capacity < 9) {
    if (capacity)
      output[0] = '\0';
    return;
  }

  const uint32_t elapsed_seconds = elapsed_ms / 1000;
  const uint32_t hours = elapsed_seconds / 3600;
  const uint32_t minutes = (elapsed_seconds / 60) % 60;
  const uint32_t seconds = elapsed_seconds % 60;
  size_t index = format_number(hours, output, capacity);
  output[index++] = ':';
  output[index++] = static_cast<char>('0' + minutes / 10);
  output[index++] = static_cast<char>('0' + minutes % 10);
  output[index++] = ':';
  output[index++] = static_cast<char>('0' + seconds / 10);
  output[index++] = static_cast<char>('0' + seconds % 10);
  output[index] = '\0';
}

void run_task_manager() {
  Kernel::Graphics::VirtualFramebuffer vfb;
  if (!vfb.open_window(640, 480, 480, 360))
    return;

  Kernel::Scheduler::ApplicationTaskInfo tasks[MAX_TASKS]{};
  size_t heap_usage[MAX_TASKS]{};
  constexpr int32_t ROW_HEIGHT = 11;

  while (!Kernel::Scheduler::should_stop()) {
    while (Kernel::Drivers::Keyboard::available()) {
      const char key = Kernel::Drivers::Keyboard::getchar();
      if (key == 27 || key == 'q' || key == 'Q')
        return;
    }

    const size_t task_count = Kernel::Scheduler::get_application_tasks(
        tasks, sizeof(tasks) / sizeof(tasks[0]));
    for (size_t i = 0; i < task_count; ++i)
      heap_usage[i] = Kernel::Memory::g_kheap.get_thread_allocated_size(
          tasks[i].id);

    vfb.clear(COLOR_BACKGROUND);
    Kernel::AppCommon::text(vfb, 12, 10, "TASK MANAGER", COLOR_TEXT);
    Kernel::AppCommon::text(vfb, 12, 30, "PID", COLOR_MUTED);
    Kernel::AppCommon::text(vfb, 68, 30, "NAME", COLOR_MUTED);
    Kernel::AppCommon::text(vfb, 250, 30, "HEAP KiB", COLOR_MUTED);
    Kernel::AppCommon::text(vfb, 390, 30, "UPTIME", COLOR_MUTED);
    vfb.fill_rect(12, 44, static_cast<int32_t>(vfb.width()) - 24, 1,
                  COLOR_HEADER);

    const size_t visible_rows =
        (vfb.height() > 52) ? (vfb.height() - 52) / ROW_HEIGHT : 0;
    const size_t shown_tasks = task_count < visible_rows ? task_count
                                                         : visible_rows;
    const uint32_t now = Kernel::Drivers::PIT::get_ticks();
    for (size_t i = 0; i < shown_tasks; ++i) {
      const int32_t y = 50 + static_cast<int32_t>(i) * ROW_HEIGHT;
      char id_text[24];
      char heap_text[24];
      char uptime_text[24];
      format_number(tasks[i].id, id_text, sizeof(id_text));
      format_number(heap_usage[i] / 1024, heap_text, sizeof(heap_text));
      format_uptime(now - tasks[i].start_ticks, uptime_text,
                    sizeof(uptime_text));

      Kernel::AppCommon::text(vfb, 12, y, id_text, COLOR_TEXT);
      Kernel::AppCommon::text(vfb, 68, y, tasks[i].name, COLOR_TEXT);
      Kernel::AppCommon::text(vfb, 250, y, heap_text, COLOR_TEXT);
      Kernel::AppCommon::text(vfb, 390, y, uptime_text, COLOR_TEXT);
    }

    vfb.flush();
    Kernel::Scheduler::thread_sleep_ms(1000);
  }
}

} // namespace

void task_manager_run() { run_task_manager(); }
} // namespace Kernel::Apps