#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/drivers/pci.h>
#include <kernel/drivers/tsc_deadline.h>
#include <kernel/crypto/password.h>
#include <kernel/debug/debug.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/uicommon.h>
#include <kernel/fs/laziofs.h>
#include <kernel/lib/appcommon.h>
#include <kernel/lib/math.h>
#include <kernel/memory/heap.h>
#include <kernel/net/smoltcp_bridge.h>
#include <kernel/scheduler.h>

namespace Kernel::Apps {
namespace {
using Kernel::UICommon::text;
using Kernel::Graphics::VirtualFramebuffer;

constexpr uint32_t BG = 0x171b22;
constexpr uint32_t PANEL = 0x222a33;
constexpr uint32_t FG = 0xe7edf2;
constexpr uint32_t MUTED = 0x9aa9b6;
constexpr uint32_t CYAN = 0x65c6c4;
constexpr uint32_t GREEN = 0x9fd6a2;
constexpr uint32_t AMBER = 0xe8bd75;
constexpr uint32_t RED = 0xe27d72;
constexpr uint32_t RUN_MS = 1000;
constexpr size_t TEST_COUNT = 31;

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

void log_result(const char *name, const Result &result) {
  char message[Kernel::Debug::LOG_MESSAGE_SIZE] = "[B] ";
  size_t index = 4;
  while (*name && index + 1 < sizeof(message))
    message[index++] = *name++;
  const char *separator = result.complete ? " rate=" : " skipped";
  while (*separator && index + 1 < sizeof(message))
    message[index++] = *separator++;
  if (result.complete) {
    char number[24];
    const size_t length = format_u64(result.rate, number, sizeof(number));
    for (size_t i = 0; i < length && index + 1 < sizeof(message); ++i)
      message[index++] = number[i];
    const char *suffix = "/s elapsed_ms=";
    while (*suffix && index + 1 < sizeof(message))
      message[index++] = *suffix++;
    const size_t elapsed_length =
        format_u64(result.elapsed_ms, number, sizeof(number));
    for (size_t i = 0; i < elapsed_length && index + 1 < sizeof(message); ++i)
      message[index++] = number[i];
  }
  message[index] = '\0';
  Kernel::Debug::log(message);
}

void keyboard_probe() {
  (void)Kernel::Drivers::Keyboard::available();
}

void mouse_probe() {
  (void)Kernel::Drivers::Mouse::get_x();
  (void)Kernel::Drivers::Mouse::get_y();
}

void pci_probe() {
  (void)pci_read_config(0, 0, 0, 0);
}

void all_driver_probe() {
  keyboard_probe();
  mouse_probe();
  pci_probe();
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
      all_driver_probe();
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
  const uint32_t elapsed = Kernel::Drivers::TSCDeadline::get_ticks() - start;
  return elapsed ? elapsed : 1;
}

Result measure_driver(void (*probe)()) {
  Result result{};
  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  uint64_t probes = 0;
  while (Kernel::Drivers::TSCDeadline::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop()) {
    probe();
    ++probes;
  }
  result.elapsed_ms = elapsed_since(start);
  result.rate = probes * 1000 / result.elapsed_ms;
  result.complete = true;
  return result;
}

Result measure_vfs() {
  Result result{};
  if (!Kernel::FS::VFS::is_mounted())
    return result;

  Kernel::FS::VFSFile file{};
  bool read_file = false;
  for (uint32_t i = 0; i < Kernel::FS::VFS_MAX_INODES; ++i) {
    Kernel::FS::VFSFile candidate{};
    if (Kernel::FS::VFS::get_file_at(i, &candidate) &&
        !candidate.is_directory && candidate.size) {
      file = candidate;
      read_file = true;
      break;
    }
  }
  if (!read_file && !Kernel::FS::VFS::find_file("/", &file))
    return result;

  uint8_t buffer[64];
  const uint32_t bytes_to_read =
      read_file && file.size < sizeof(buffer) ? file.size : sizeof(buffer);
  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  uint64_t operations = 0;
  bool valid = true;
  while (Kernel::Drivers::TSCDeadline::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop()) {
    Kernel::FS::VFSFile current{};
    if (!Kernel::FS::VFS::find_file(file.name, &current)) {
      valid = false;
      break;
    }
    if (read_file) {
      if (Kernel::FS::VFS::read_file(&current, buffer, bytes_to_read, 0) !=
          static_cast<int>(bytes_to_read)) {
        valid = false;
        break;
      }
    } else if (!Kernel::FS::VFS::vfs_exists("/")) {
      valid = false;
      break;
    }
    ++operations;
  }
  result.elapsed_ms = elapsed_since(start);
  result.rate = operations * 1000 / result.elapsed_ms;
  result.complete = valid;
  return result;
}

Result measure_graphics(VirtualFramebuffer &vfb) {
  Result result{};
  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  uint64_t batches = 0;
  while (Kernel::Drivers::TSCDeadline::get_ticks() - start < RUN_MS &&
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

  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  while (Kernel::Drivers::TSCDeadline::get_ticks() - start < RUN_MS &&
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

  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  while (Kernel::Drivers::TSCDeadline::get_ticks() - start < RUN_MS &&
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

Result measure_heap() {
  Result result{};
  auto &heap = Kernel::Memory::g_kheap;
  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  uint64_t batches = 0;
  bool valid = true;
  while (Kernel::Drivers::TSCDeadline::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop()) {
    void *blocks[32]{};
    for (size_t i = 0; i < 32; ++i) {
      const size_t size = 16 + ((i * 37 + batches) % 240);
      blocks[i] = heap.kmalloc(size);
      if (!blocks[i]) {
        valid = false;
        break;
      }
      static_cast<uint8_t *>(blocks[i])[0] = static_cast<uint8_t>(i);
    }
    for (size_t i = 0; i < 32; ++i) {
      if (blocks[i] && i % 4 == 0) {
        void *resized = heap.krealloc(blocks[i], 384 + i);
        if (resized)
          blocks[i] = resized;
        else
          valid = false;
      }
    }
    for (void *block : blocks)
      if (block)
        heap.kfree(block);
    if (!valid)
      break;
    ++batches;
  }
  result.elapsed_ms = elapsed_since(start);
  valid = valid && heap.validate();
  result.rate = batches * 1000 / result.elapsed_ms;
  result.complete = valid;
  return result;
}

Result measure_math() {
  Result result{};
  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  uint64_t iterations = 0;
  volatile uint64_t checksum = 0;
  while (Kernel::Drivers::TSCDeadline::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop()) {
    const uint64_t value = 1000000 + (iterations & 0xffff);
    checksum += Kernel::Lib::Math::isqrt_u64(value);
    checksum += static_cast<uint32_t>(Kernel::Lib::Math::fixed_sin(
        static_cast<int32_t>(iterations % 360)));
    checksum += static_cast<uint32_t>(Kernel::Lib::Math::fixed_cos(
        static_cast<int32_t>((iterations * 7) % 360)));
    ++iterations;
  }
  (void)checksum;
  result.elapsed_ms = elapsed_since(start);
  result.rate = iterations * 1000 / result.elapsed_ms;
  result.complete = true;
  return result;
}

Result measure_software_interrupt() {
  Result result{};
  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  uint64_t entries = 0;
  while (Kernel::Drivers::TSCDeadline::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop()) {
    __asm__ volatile("int $0x30" ::: "memory");
    ++entries;
  }
  result.elapsed_ms = elapsed_since(start);
  result.rate = entries * 1000 / result.elapsed_ms;
  result.complete = true;
  return result;
}

// TSCDeadline::tick is updated by the timer IRQ handler; this measures delivered ticks.
Result measure_timer_interrupts() {
  Result result{};
  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  const uint32_t start_interrupts =
      Kernel::Drivers::TSCDeadline::get_interrupt_count();
  while (Kernel::Drivers::TSCDeadline::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop())
    Kernel::Scheduler::thread_yield();
  result.elapsed_ms = elapsed_since(start);
  result.rate = static_cast<uint64_t>(
                    Kernel::Drivers::TSCDeadline::get_interrupt_count() -
                    start_interrupts) *
                1000 / result.elapsed_ms;
  result.complete = true;
  return result;
}

Result measure_smoltcp_poll() {
  Result result{};
  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  uint64_t polls = 0;
  while (Kernel::Drivers::TSCDeadline::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop()) {
    if (Kernel::Net::poll() < 0)
      break;
    ++polls;
  }
  result.elapsed_ms = elapsed_since(start);
  result.rate = polls * 1000 / result.elapsed_ms;
  result.complete = polls != 0;
  return result;
}

// fetch() uses BearSSL for HTTPS and the smoltcp TCP transport. It is a
// single end-to-end operation, so report its measured duration as one fetch.
Result measure_bearssl_https() {
  Result result{};
  char response[256];
  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  const int fetched = Kernel::Net::fetch("https://example.com/", response,
                                        sizeof(response));
  result.elapsed_ms = elapsed_since(start);
  result.rate = fetched >= 0 ? 1000 / result.elapsed_ms : 0;
  result.complete = fetched >= 0;
  return result;
}

// Extended microbenchmarks use bounded working sets and avoid persistent I/O.
// Each case runs for the same wall-clock interval as the original suite.
Result measure_extended(VirtualFramebuffer &vfb, size_t test_id) {
  Result result{};
  const uint32_t start = Kernel::Drivers::TSCDeadline::get_ticks();
  uint64_t operations = 0;
  volatile uint64_t checksum = 0;
  bool valid = true;
  uint32_t pixels[256];
  for (size_t i = 0; i < 256; ++i)
    pixels[i] = static_cast<uint32_t>(i * 2654435761u);
  auto &heap = Kernel::Memory::g_kheap;
  while (Kernel::Drivers::TSCDeadline::get_ticks() - start < RUN_MS &&
         !Kernel::Scheduler::should_stop() && valid) {
    switch (test_id) {
    case 8: // sequential memory read
      for (size_t i = 0; i < 256; ++i) checksum += pixels[i];
      operations += 256;
      break;
    case 9: // memory write and readback
      for (size_t i = 0; i < 256; ++i) pixels[i] = pixels[i] * 33u + 17u;
      checksum += pixels[operations & 255]; operations += 256;
      break;
    case 10: // allocator small-object churn
    case 11: { // allocator mixed-size churn
      void *blocks[32]{};
      size_t requested[32]{};
      for (size_t i = 0; i < 32; ++i) {
        requested[i] = test_id == 10 ? 32 : 64 + i * 31;
        blocks[i] = heap.kmalloc(requested[i]);
        if (!blocks[i]) {
          valid = false;
          break;
        }
        auto *bytes = static_cast<uint8_t *>(blocks[i]);
        bytes[0] = static_cast<uint8_t>(i ^ 0xa5);
        bytes[requested[i] - 1] = static_cast<uint8_t>(i ^ 0x5a);
        ++operations;
      }
      for (size_t i = 0; i < 32; ++i) {
        if (!blocks[i])
          continue;
        auto *bytes = static_cast<uint8_t *>(blocks[i]);
        if (bytes[0] != static_cast<uint8_t>(i ^ 0xa5) ||
            bytes[requested[i] - 1] != static_cast<uint8_t>(i ^ 0x5a))
          valid = false;
        heap.kfree(blocks[i]);
      }
      if (!valid)
        break;
      break;
    }
    case 12: // framebuffer fill bandwidth
      vfb.fill(static_cast<uint32_t>(operations)); operations += vfb.width() * vfb.height();
      break;
    case 13: // clipped rectangle rasterization
      vfb.fill_rect(-20, 20, static_cast<int32_t>(vfb.width()), 80, CYAN); ++operations;
      break;
    case 14: // line rasterization
      vfb.draw_line(0, operations % vfb.height(), vfb.width() - 1,
                    (operations * 7) % vfb.height(), AMBER); ++operations;
      break;
    case 15: // circle rasterization
      vfb.fill_circle(vfb.width()/2, vfb.height()/2, 60, GREEN); ++operations;
      break;
    case 16: // alpha blend rectangle
      vfb.fill_rect_alpha(30, 30, 300, 180, RED, operations % 256); ++operations;
      break;
    case 17: // glyph rendering
      vfb.draw_glyph(operations % (vfb.width() - 8), 80, 'A' + operations % 26, FG); ++operations;
      break;
    case 18: // opaque blit
      vfb.blit(pixels, 0, 0, 16, 16, 16); operations += 256;
      break;
    case 19: // transparent blit
      vfb.blit_transparent(pixels, 0, 0, 16, 16, 16, 0); operations += 256;
      break;
    case 20: // alpha blit
      vfb.blit_alpha(pixels, 0, 0, 16, 16, 16, 128); operations += 256;
      break;
    case 21: // triangle fill
      vfb.fill_triangle(20, 20, 300, 100, 100, 300, CYAN); ++operations;
      break;
    case 22: // rounded rectangles
      vfb.fill_rounded_rect(20, 20, 240, 120, 24, AMBER); ++operations;
      break;
    case 23: // sectors and arcs
      vfb.fill_sector(200, 150, 100, operations % 180, operations % 180 + 120, GREEN); ++operations;
      break;
    case 24: // password hash workload
      checksum += Kernel::Crypto::hash_password("OSFOS benchmark workload", 24,
                                                0x9e3779b97f4a7c15ULL + operations);
      ++operations;
      break;
    default: // integer and fixed-point math mix
      checksum += Kernel::Lib::Math::isqrt_u64(operations * 7919 + 1);
      checksum += static_cast<uint32_t>(Kernel::Lib::Math::fixed_sin(operations % 360));
      ++operations;
      break;
    }
    if (!valid)
      break;
  }
  (void)checksum;
  result.elapsed_ms = elapsed_since(start);
  if ((test_id == 10 || test_id == 11) && !heap.validate())
    valid = false;
  result.rate = operations * 1000 / result.elapsed_ms;
  result.complete = valid;
  return result;
}

void render(VirtualFramebuffer &vfb, size_t app_count) {
  const int32_t width = static_cast<int32_t>(vfb.width());
  const int32_t height = static_cast<int32_t>(vfb.height());
  vfb.clear(BG);
  Kernel::UICommon::text(vfb, 18, 12, "SYSTEM BENCHMARK", FG);
  Kernel::UICommon::text(vfb, 18, 28, "OSFOS / LIVE KERNEL MEASUREMENTS", MUTED);
  vfb.fill_rect(18, 44, width - 36, 1, PANEL);
  Kernel::UICommon::text(vfb, 18, 54, running ? "RUNNING  |  31 TESTS" : "31 TESTS  |  CLICK ROW OR SPACE FOR ALL", running ? AMBER : CYAN);
  const char *names[TEST_COUNT] = {
      "01 KEYBOARD DRIVER", "02 MOUSE DRIVER", "03 PCI CONFIG READ",
      "04 FRAMEBUFFER DRAW + FLUSH", "05 SCHEDULER HANDOFFS",
      "06 CONCURRENT WORKFLOW", "07 HEAP REALLOC / FREE", "08 INTEGER MATH",
      "09 SEQUENTIAL MEMORY READ", "10 MEMORY WRITE + READBACK",
      "11 SMALL ALLOCATOR CHURN", "12 MIXED ALLOCATOR CHURN",
      "13 FRAMEBUFFER FILL", "14 CLIPPED RECTANGLES", "15 LINE RASTERIZATION",
      "16 FILLED CIRCLES", "17 ALPHA RECTANGLES", "18 GLYPH RENDERING",
      "19 OPAQUE BLIT", "20 TRANSPARENT BLIT", "21 ALPHA BLIT",
      "22 TRIANGLE FILL", "23 ROUNDED RECTANGLES", "24 ARC / SECTOR RASTER",
      "25 PASSWORD HASH", "26 FIXED-POINT MATH", "27 VFS READ / LOOKUP",
      "28 SOFTWARE INTERRUPT / SCHEDULER TRAP", "29 PIT TIMER IRQ RATE",
      "30 SMOLTCP POLL", "31 BEARSSL HTTPS FETCH"};
  const uint32_t colors[TEST_COUNT] = {
      CYAN, GREEN, AMBER, GREEN, AMBER, RED, AMBER, CYAN, GREEN, GREEN, AMBER,
      AMBER, GREEN, CYAN, CYAN, GREEN, RED, FG, CYAN, CYAN, RED, AMBER, AMBER,
      GREEN, RED, CYAN, AMBER, RED, AMBER, CYAN, GREEN};
  uint64_t max_rate = 1;
  for (size_t i = 0; i < TEST_COUNT; ++i) {
    max_rate = std::max(results[i].rate, max_rate);
  }

  const int32_t bar_width = width - 36;
  for (size_t i = 0; i < TEST_COUNT; ++i) {
    const int32_t row_y = 78 + static_cast<int32_t>(i) * 12;
    Kernel::UICommon::text(vfb, 18, row_y, names[i], FG);
    if (results[i].complete) {
      char value[32];
      format_rate(results[i].rate, value, sizeof(value));
      const int32_t value_x = width - 72;
      Kernel::UICommon::text(vfb, value_x, row_y, value, colors[i]);
      const uint64_t scaled = results[i].rate * static_cast<uint64_t>(bar_width) /
                              max_rate;
      vfb.fill_rect(18, row_y + 9,
                    static_cast<int32_t>(scaled ? scaled : 1), 2, colors[i]);
    } else {
      Kernel::UICommon::text(vfb, width - 42, row_y, "--", MUTED);
    }
  }
  (void)app_count;
  Kernel::UICommon::text(vfb, 18, height - 22, "SPACE RUN ALL  R RESET  Q / ESC CLOSE", CYAN);
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
    results[test_id] = measure_driver(keyboard_probe);
  else if (test_id == 1)
    results[test_id] = measure_driver(mouse_probe);
  else if (test_id == 2)
    results[test_id] = measure_driver(pci_probe);
  else if (test_id == 3)
    results[test_id] = measure_graphics(vfb);
  else if (test_id == 4)
    results[test_id] = measure_context();
  else if (test_id == 5)
    results[test_id] = measure_workflow();
  else if (test_id == 6)
    results[test_id] = measure_heap();
  else if (test_id == 7)
    results[test_id] = measure_math();
  else if (test_id == 26)
    results[test_id] = measure_vfs();
  else if (test_id == 27)
    results[test_id] = measure_software_interrupt();
  else if (test_id == 28)
    results[test_id] = measure_timer_interrupts();
  else if (test_id == 29)
    results[test_id] = measure_smoltcp_poll();
  else if (test_id == 30)
    results[test_id] = measure_bearssl_https();
  else
    results[test_id] = measure_extended(vfb, test_id);
  const char *names[TEST_COUNT] = {
      "keyboard", "mouse", "pci", "graphics", "scheduler", "workflow",
      "heap", "math", "mem_read", "mem_rw", "heap_small", "heap_mixed",
      "fb_fill", "fb_rect", "fb_line", "fb_circle", "fb_alpha", "fb_glyph",
      "fb_blit", "fb_blit_key", "fb_blit_alpha", "fb_triangle", "fb_round",
      "fb_sector", "password_hash", "math_fixed", "vfs", "soft_interrupt",
      "pit_timer_irq", "smoltcp_poll", "bearssl_https"};
  log_result(names[test_id], results[test_id]);
  running = false;
}

void run_benchmark() {
  VirtualFramebuffer vfb;
  if (!vfb.open_window(800, 600, 560, 450))
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
      } else if (key >= '1' && key <= '9') {
        selected = key - '1';
      } else if (key == 'r') {
        clear_results();
      }
    }

    const bool mouse_down = Kernel::UICommon::primary_pressed();
    if (mouse_down && !previous_mouse) {
      int window_x = 0, window_y = 0;
      if (Kernel::AppCommon::get_window_position(
              Kernel::Scheduler::current_thread_id(), &window_x, &window_y)) {
        const int32_t mouse_x = Kernel::UICommon::mouse_x() - window_x;
        const int32_t mouse_y = Kernel::UICommon::mouse_y() - window_y;
        if (mouse_x >= 18 && mouse_x < static_cast<int32_t>(vfb.width()) - 18 &&
            mouse_y >= 78 && mouse_y < 78 + static_cast<int32_t>(TEST_COUNT) * 12)
          selected = (mouse_y - 78) / 12;
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

void run_startup_benchmark() {
  Kernel::Debug::log("[B] startup benchmark begin (graphics skipped)");
  results[0] = measure_driver(keyboard_probe);
  log_result("keyboard", results[0]);
  results[1] = measure_driver(mouse_probe);
  log_result("mouse", results[1]);
  results[2] = measure_driver(pci_probe);
  log_result("pci", results[2]);
  results[4] = measure_context();
  log_result("scheduler", results[4]);
  results[5] = measure_workflow();
  log_result("workflow", results[5]);
  results[6] = measure_heap();
  log_result("heap", results[6]);
  results[7] = measure_math();
  log_result("math", results[7]);
  results[26] = measure_vfs();
  log_result("vfs", results[26]);
  results[27] = measure_software_interrupt();
  log_result("soft_interrupt", results[27]);
  results[28] = measure_timer_interrupts();
  log_result("pit_timer_irq", results[28]);
  Kernel::Debug::log("[B] startup benchmark complete");
}
} // namespace

void benchmark_run() { run_benchmark(); }
void benchmark_startup_run() { run_startup_benchmark(); }
} // namespace Kernel::Apps
