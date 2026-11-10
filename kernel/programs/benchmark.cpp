#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/drivers/pci.h>
#include <kernel/drivers/pit.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/appcommon.h>
#include <kernel/scheduler.h>

namespace Kernel::Apps {
namespace {
using Kernel::AppCommon::text;
using Kernel::Graphics::VirtualFramebuffer;

constexpr uint32_t BG = 0x171b22;
constexpr uint32_t PANEL = 0x222a33;
constexpr uint32_t FG = 0xe7edf2;
constexpr uint32_t MUTED = 0x9aa9b6;
constexpr uint32_t CYAN = 0x65c6c4;
constexpr uint32_t GREEN = 0x9fd6a2;
constexpr uint32_t AMBER = 0xe8bd75;
constexpr uint32_t RED = 0xe27d72;
constexpr uint32_t RUN_MS = 5000;
constexpr size_t TEST_COUNT = 4;

struct Result {
  uint64_t rate = 0;
  uint32_t elapsed_ms = 0;
  bool complete = false;
};

struct WorkflowState {
  volatile bool active = false;
  volatile bool done[3] = {true, true, true};
  volatile uint64_t counts[3] = {0, 0, 0};
};

struct ContextState {
  volatile bool active = false;
  volatile bool done = true;
  volatile uint64_t handoffs = 0;
};

Result results[TEST_COUNT];
WorkflowState workflow;
ContextState context;
bool running = false;

size_t format_u64(uint64_t value, char *output, size_t capacity) {
  if (capacity == 0)
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

void format_rate(uint64_t rate, char *output, size_t capacity) {
  size_t index = format_u64(rate, output, capacity);
  if (index + 2 < capacity) {
    output[index++] = '/';
    output[index++] = 's';
    output[index] = '\0';
  }
}

void pci_input_probe() {
  (void)Kernel::Drivers::Keyboard::available();
  (void)Kernel::Drivers::Mouse::get_x();
  (void)Kernel::Drivers::Mouse::get_y();
  (void)pci_read_config(0, 0, 0, 0);
}

void draw_probe(VirtualFramebuffer &vfb, uint32_t frame) {
  const int32_t width = static_cast<int32_t>(vfb.width());
  const int32_t height = static_cast<int32_t>(vfb.height());
  const int32_t x = 20 + static_cast<int32_t>(frame % 9) * 13;
  const int32_t y = 110 + static_cast<int32_t>(frame % 7) * 11;
  vfb.fill_rect(x, y, 34, 20, CYAN);
  vfb.draw_line(x, y + 24, x + 42, y + 8, AMBER);
  vfb.fill_circle_alpha(width - 48, height / 2, 15, GREEN, 170);
}

void context_worker() {
  while (context.active) {
    ++context.handoffs;
    Kernel::Scheduler::thread_yield();
  }
  context.done = true;
  Kernel::Scheduler::thread_exit();
}

void workflow_worker(void *argument) {
  const size_t worker_id = reinterpret_cast<uintptr_t>(argument);
  uint32_t pixel = static_cast<uint32_t>(worker_id + 1);
  volatile uint32_t pixels[256]{};
  while (workflow.active) {
    if (worker_id == 0) {
      pci_input_probe();
    } else if (worker_id == 1) {
      for (size_t i = 0; i < 256; ++i) {
        pixel = pixel * 1664525u + 1013904223u;
        pixels[i] = pixel;
      }
    } else {
      Kernel::Scheduler::thread_yield();
    }
    ++workflow.counts[worker_id];
    if (worker_id != 2)
      Kernel::Scheduler::thread_yield();
  }
  workflow.done[worker_id] = true;
  Kernel::Scheduler::thread_exit();
}

uint32_t elapsed_since(uint32_t start) {
  const uint32_t elapsed = Kernel::Drivers::PIT::get_ticks() - start;
  return elapsed ? elapsed : 1;
}

Result measure_drivers() {
  Result result{};
  const uint32_t start = Kernel::Drivers::PIT::get_ticks();
  uint64_t probes = 0;
  while (Kernel::Drivers::PIT::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop()) {
    pci_input_probe();
    ++probes;
  }
  result.elapsed_ms = elapsed_since(start);
  result.rate = probes * 1000 / result.elapsed_ms;
  result.complete = true;
  return result;
}

Result measure_graphics(VirtualFramebuffer &vfb) {
  Result result{};
  const uint32_t start = Kernel::Drivers::PIT::get_ticks();
  uint64_t batches = 0;
  while (Kernel::Drivers::PIT::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop()) {
    draw_probe(vfb, static_cast<uint32_t>(batches));
    vfb.flush();
    ++batches;
  }
  result.elapsed_ms = elapsed_since(start);
  result.rate = batches * 1000 / result.elapsed_ms;
  result.complete = true;
  return result;
}

Result measure_context() {
  Result result{};
  context.handoffs = 0;
  context.done = false;
  context.active = true;
  if (Kernel::Scheduler::thread_create(context_worker) < 0) {
    context.active = false;
    context.done = true;
    return result;
  }

  const uint32_t start = Kernel::Drivers::PIT::get_ticks();
  while (Kernel::Drivers::PIT::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop())
    Kernel::Scheduler::thread_yield();
  result.elapsed_ms = elapsed_since(start);
  context.active = false;
  while (!context.done)
    Kernel::Scheduler::thread_yield();
  result.rate = context.handoffs * 1000 / result.elapsed_ms;
  result.complete = true;
  return result;
}

Result measure_workflow() {
  Result result{};
  workflow.active = true;
  for (size_t i = 0; i < 3; ++i) {
    workflow.done[i] = false;
    workflow.counts[i] = 0;
  }

  size_t started = 0;
  for (; started < 3; ++started) {
    if (Kernel::Scheduler::thread_create(
            workflow_worker, reinterpret_cast<void *>(started)) < 0)
      break;
  }
  if (started != 3) {
    workflow.active = false;
    while (started > 0) {
      --started;
      while (!workflow.done[started])
        Kernel::Scheduler::thread_yield();
    }
    return result;
  }

  const uint32_t start = Kernel::Drivers::PIT::get_ticks();
  while (Kernel::Drivers::PIT::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop())
    Kernel::Scheduler::thread_yield();
  result.elapsed_ms = elapsed_since(start);
  workflow.active = false;
  for (size_t i = 0; i < 3; ++i) {
    while (!workflow.done[i])
      Kernel::Scheduler::thread_yield();
  }
  const uint64_t operations =
      workflow.counts[0] + workflow.counts[1] + workflow.counts[2];
  result.rate = operations * 1000 / result.elapsed_ms;
  result.complete = true;
  return result;
}

void draw_button(VirtualFramebuffer &vfb, int32_t x, int32_t y, int32_t w,
                 const char *label, uint32_t color) {
  vfb.fill_rect(x, y, w, 24, PANEL);
  vfb.fill_rect(x, y, 3, 24, color);
  text(vfb, x + 10, y + 8, label, FG);
}

void render(VirtualFramebuffer &vfb, size_t app_count) {
  const int32_t width = static_cast<int32_t>(vfb.width());
  const int32_t height = static_cast<int32_t>(vfb.height());
  vfb.clear(BG);
  text(vfb, 18, 12, "SYSTEM BENCHMARK", FG);
  text(vfb, 18, 28, "OSFOS / LIVE KERNEL MEASUREMENTS", MUTED);
  vfb.fill_rect(18, 44, width - 36, 1, PANEL);
  text(vfb, 18, 54, running ? "TEST RUNNING" : "READY", running ? AMBER : GREEN);
  text(vfb, width - 176, 54, "300 ms per test", MUTED);

  const int32_t gap = 8;
  const int32_t button_width = (width - 36 - gap * 3) / 4;
  const char *labels[4] = {"1 DRIVERS", "2 GRAPHICS", "3 SCHEDULER",
                           "4 WORKFLOW"};
  const uint32_t colors[4] = {CYAN, GREEN, AMBER, RED};
  for (size_t i = 0; i < TEST_COUNT; ++i)
    draw_button(vfb, 18 + static_cast<int32_t>(i) * (button_width + gap), 76,
                button_width, labels[i], colors[i]);

  text(vfb, 18, 116, "MEASUREMENTS", MUTED);
  const char *names[4] = {"PCI + INPUT DRIVER PROBES", "FRAMEBUFFER DRAW + FLUSH",
                          "SCHEDULER HANDOFFS", "CONCURRENT WORKFLOW"};
  const char *units[4] = {"probes/s", "frames/s", "handoffs/s", "ops/s"};
  uint64_t max_rate = 1;
  for (size_t i = 0; i < TEST_COUNT; ++i) {
    max_rate = std::max(results[i].rate, max_rate);
  }

  const int32_t row_y[4] = {138, 190, 242, 294};
  const int32_t bar_width = width - 36;
  for (size_t i = 0; i < TEST_COUNT; ++i) {
    text(vfb, 18, row_y[i], names[i], FG);
    if (results[i].complete) {
      char value[32];
      format_rate(results[i].rate, value, sizeof(value));
      const int32_t value_x = width - 150;
      text(vfb, value_x, row_y[i], value, colors[i]);
      text(vfb, width - 78, row_y[i], units[i], MUTED);
      const uint64_t scaled = results[i].rate * static_cast<uint64_t>(bar_width) /
                              max_rate;
      vfb.fill_rect(18, row_y[i] + 20,
                    static_cast<int32_t>(scaled ? scaled : 1), 4, colors[i]);
    } else {
      text(vfb, width - 92, row_y[i], "--", MUTED);
    }
    if (i < TEST_COUNT - 1)
      vfb.fill_rect(18, row_y[i] + 39, width - 36, 1, PANEL);
  }

  text(vfb, 18, height - 58, "OPEN APPS", MUTED);
  char app_text[24];
  format_u64(app_count, app_text, sizeof(app_text));
  text(vfb, 106, height - 58, app_text, FG);
  text(vfb, 168, height - 58, "3 WORKERS IN WORKFLOW MODE", MUTED);
  text(vfb, 18, height - 40, "WORKFLOW: PCI / INPUT POLLING + CPU DRAW + CONTEXT LOAD",
       FG);
  text(vfb, 18, height - 22, "SPACE RUN ALL   R RESET   Q / ESC CLOSE", CYAN);
  vfb.flush();
}

void clear_results() {
  for (size_t i = 0; i < TEST_COUNT; ++i)
    results[i] = Result{};
}

void run_selected(VirtualFramebuffer &vfb, size_t test_id, size_t app_count) {
  running = true;
  render(vfb, app_count);
  if (test_id == 0)
    results[test_id] = measure_drivers();
  else if (test_id == 1)
    results[test_id] = measure_graphics(vfb);
  else if (test_id == 2)
    results[test_id] = measure_context();
  else
    results[test_id] = measure_workflow();
  running = false;
}

void run_benchmark() {
  VirtualFramebuffer vfb;
  if (!vfb.open_window(800, 600, 560, 420))
    return;

  bool previous_mouse = false;
  while (!Kernel::Scheduler::should_stop()) {
    Kernel::Scheduler::ApplicationTaskInfo app_tasks[
        Kernel::Scheduler::MAX_APPLICATION_TASKS]{};
    const size_t app_count = Kernel::Scheduler::get_application_tasks(
        app_tasks, Kernel::Scheduler::MAX_APPLICATION_TASKS);
    render(vfb, app_count);

    bool run_all = false;
    int selected = -1;
    while (Kernel::Drivers::Keyboard::available()) {
      char key = Kernel::Drivers::Keyboard::getchar();
      if (key >= 'A' && key <= 'Z')
        key += 'a' - 'A';
      if (key == 'q' || key == 27)
        return;
      if (key == ' ') {
        run_all = true;
      } else if (key >= '1' && key <= '4') {
        selected = key - '1';
      } else if (key == 'r') {
        clear_results();
      }
    }

    const bool mouse_down = Kernel::AppCommon::is_left_pressed();
    if (mouse_down && !previous_mouse) {
      int window_x = 0, window_y = 0;
      if (Kernel::AppCommon::get_window_position(
              Kernel::Scheduler::current_thread_id(), &window_x, &window_y)) {
        const int32_t mouse_x = Kernel::AppCommon::get_mouse_x() - window_x;
        const int32_t mouse_y = Kernel::AppCommon::get_mouse_y() - window_y;
        const int32_t button_width =
            (static_cast<int32_t>(vfb.width()) - 60) / 4;
        if (mouse_y >= 76 && mouse_y < 100 && mouse_x >= 18 &&
            mouse_x < static_cast<int32_t>(vfb.width()) - 18) {
          selected = (mouse_x - 18) / (button_width + 8);
          if (selected >= 4)
            selected = -1;
        }
      }
    }
    previous_mouse = mouse_down;

    if (run_all) {
      clear_results();
      for (size_t i = 0; i < TEST_COUNT &&
                         !Kernel::Scheduler::should_stop();
           ++i) {
        run_selected(vfb, i, app_count);
      }
    } else if (selected >= 0 && selected < static_cast<int>(TEST_COUNT)) {
      run_selected(vfb, static_cast<size_t>(selected), app_count);
    }
    Kernel::Scheduler::thread_sleep_ms(30);
  }
}
} // namespace

void benchmark_run() { run_benchmark(); }
} // namespace Kernel::Apps
