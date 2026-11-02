#include <arch/x86_64/string.h>
#include <kernel/drivers/ata.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/pci.h>
#include <kernel/drivers/usb/xhci.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/vfb.h>
#include <kernel/graphics/window_manager.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/pmm.h>
#include <kernel/scheduler.h>
#include <kernel/shell/shell.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include <kernel/cpu.h>

namespace Kernel::Games {
namespace Spong {
void run(uint32_t speed, bool ai);
}
namespace Breakout {
void run();
}
namespace Doom {
void run(const char *wad);
}
namespace Tetris {
void run();
}
} // namespace Kernel::Games

namespace Kernel::Apps {
void graphic_test_run();
void stress_test_run();
void shitty_excel_run();
void memory_analyzer_run();
namespace Pain {
void run();
}
namespace Calc {
void run();
}
} // namespace Kernel::Apps

namespace Kernel::Shell {

// --- Constants & Theme ---
constexpr uint32_t BG = 0x1B1111;
constexpr uint32_t PANEL = 0x2E1E1E;
constexpr uint32_t FG = 0xF4D6CD;
constexpr uint32_t MUTED = 0xC8ADA6;
constexpr uint32_t BLUE = 0xFAB489;
constexpr uint32_t GREEN = 0xA1E3A6;
constexpr uint32_t RED = 0xA88BF3;
constexpr uint32_t YELLOW = 0xAFE2F9;

constexpr uint32_t CELL_W = 8;
constexpr uint32_t CELL_H = 8;
constexpr uint32_t LINE_H = 10;
constexpr uint32_t TOP_H = 12;
constexpr size_t MAX_COLS = 256;
constexpr size_t MAX_ROWS = 64;
constexpr size_t INPUT_SIZE = 256;

namespace {
Kernel::Graphics::VirtualFramebuffer term_vfb;
char screen[MAX_ROWS][MAX_COLS];
uint32_t screen_fg[MAX_ROWS][MAX_COLS];
size_t cols = 0, rows = 0, col = 0, row = 0;
uint32_t screen_w = 0, screen_h = 0;

struct AppLaunch {
  void (*entry)();
};

void app_thread_entry(void *argument) {
  AppLaunch *launch = static_cast<AppLaunch *>(argument);
  void (*entry)() = launch ? launch->entry : nullptr;
  if (launch)
    kfree(launch);
  while (!Kernel::Scheduler::should_stop() &&
         !Kernel::Graphics::WindowManager::has_window(
             Kernel::Scheduler::current_thread_id()))
    Kernel::Scheduler::thread_sleep_ms(1);
  if (entry)
    entry();
  Kernel::Graphics::WindowManager::unregister_window(
      Kernel::Scheduler::current_thread_id());
}

bool launch_app(const char *title, void (*entry)()) {
  AppLaunch *launch = static_cast<AppLaunch *>(kmalloc(sizeof(AppLaunch)));
  if (!launch)
    return false;
  launch->entry = entry;
  int id = Kernel::Scheduler::thread_create(app_thread_entry, launch);
  if (id < 0) {
    kfree(launch);
    return false;
  }
  if (Kernel::Graphics::WindowManager::register_window(
          static_cast<uint64_t>(id), title) < 0) {
    Kernel::Scheduler::terminate_thread(static_cast<uint64_t>(id));
    return false;
  }
  return true;
}

void run_spong() { Kernel::Games::Spong::run(2, true); }
void run_breakout() { Kernel::Games::Breakout::run(); }
void run_doom1() { Kernel::Games::Doom::run("/bin/doom/doom1.wad"); }
void run_doom2() { Kernel::Games::Doom::run("/bin/doom/doom2.wad"); }
void run_tetris() { Kernel::Games::Tetris::run(); }
void run_graphic_test() { Kernel::Apps::graphic_test_run(); }
void run_stress_test() { Kernel::Apps::stress_test_run(); }
void run_shitty_excel() { Kernel::Apps::shitty_excel_run(); }
void run_pain() { Kernel::Apps::Pain::run(); }
void run_calc() { Kernel::Apps::Calc::run(); }

inline void draw_glyph(uint32_t x, uint32_t y, char ch, uint32_t fg) {
  term_vfb.draw_glyph_bg(static_cast<int32_t>(x), static_cast<int32_t>(y), ch,
                         fg, BG);
}

void draw_text_at(uint32_t x, uint32_t y, const char *s, uint32_t color) {
  while (*s && x + CELL_W <= screen_w) {
    draw_glyph(x, y, *s++, color);
    x += CELL_W;
  }
}

void draw_header() {
  term_vfb.fill_rect(0, 0, static_cast<int32_t>(screen_w),
                     static_cast<int32_t>(TOP_H), PANEL);
  term_vfb.fill_rect(0, static_cast<int32_t>(TOP_H - 2),
                     static_cast<int32_t>(screen_w), 1, BLUE);
  draw_text_at(12, 2, "OSFOS / KERNEL SHELL", GREEN);
}

void clear_row(size_t r) {
  for (size_t c = 0; c < cols; ++c) {
    screen[r][c] = ' ';
    screen_fg[r][c] = FG;
  }
}

void redraw_terminal() {
  for (size_t r = 0; r < rows; ++r)
    for (size_t c = 0; c < cols; ++c)
      draw_glyph(c * CELL_W, TOP_H + r * LINE_H, screen[r][c], screen_fg[r][c]);
}

void scroll_one_line() {
  if (rows == 0)
    return;
  memmove(&screen[0][0], &screen[1][0], (rows - 1) * MAX_COLS * sizeof(char));
  memmove(&screen_fg[0][0], &screen_fg[1][0],
          (rows - 1) * MAX_COLS * sizeof(uint32_t));
  clear_row(rows - 1);
  row = rows - 1;
  redraw_terminal();
}

void next_line() {
  col = 0;
  if (row + 1 >= rows)
    scroll_one_line();
  else
    ++row;
}

bool equals(const char *a, const char *b) {
  while (*a && *b && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

bool parse_size(const char *s, size_t &value) {
  if (!*s)
    return false;
  value = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9')
      return false;
    size_t digit = *s - '0';
    if (value > (static_cast<size_t>(-1) - digit) / 10)
      return false;
    value = value * 10 + digit;
  }
  return value != 0;
}
} // namespace

// --- Output Functions ---
void put_char(char ch, uint32_t color) {
  if (!cols || !rows)
    return;
  if (ch == '\n') {
    next_line();
    return;
  }
  if (ch == '\r') {
    col = 0;
    return;
  }
  if (ch == '\t') {
    for (int i = 0; i < 4; ++i)
      put_char(' ', color);
    return;
  }
  if (ch == '\b') {
    if (col > 0)
      --col;
    else if (row > 0) {
      --row;
      col = cols - 1;
    } else
      return;
    screen[row][col] = ' ';
    screen_fg[row][col] = FG;
    draw_glyph(col * CELL_W, TOP_H + row * LINE_H, ' ', FG);
    return;
  }
  if (static_cast<unsigned char>(ch) < 32 ||
      static_cast<unsigned char>(ch) > 126)
    return;
  screen[row][col] = ch;
  screen_fg[row][col] = color;
  draw_glyph(col * CELL_W, TOP_H + row * LINE_H, ch, color);
  if (++col >= cols)
    next_line();
}

void print(const char *s, uint32_t color) {
  while (*s)
    put_char(*s++, color);
  term_vfb.flush();
}

void print_int(long long value, int base, bool uppercase, bool signed_val,
               int width, char pad_char, uint32_t color) {
  char buf[64];
  int i = 0;
  bool negative = false;

  if (signed_val && value < 0) {
    negative = true;
    value = -value;
  }

  if (value == 0) {
    buf[i++] = '0';
  } else {
    unsigned long long uval = value;
    while (uval > 0) {
      int rem = uval % base;
      buf[i++] =
          (rem < 10) ? ('0' + rem) : ((uppercase ? 'A' : 'a') + (rem - 10));
      uval /= base;
    }
  }

  int num_chars = i + (negative ? 1 : 0);
  if (pad_char == '0') {
    if (negative) {
      put_char('-', color);
      width--;
    }
    while (width-- > i)
      put_char('0', color);
    while (i > 0)
      put_char(buf[--i], color);
  } else {
    int total_len = (num_chars > width) ? num_chars : width;
    for (int p = total_len - num_chars; p > 0; --p)
      put_char(' ', color);
    if (negative)
      put_char('-', color);
    while (i > 0)
      put_char(buf[--i], color);
  }
}

namespace {
// --- Command Implementations ---
void prompt() {
  print("root@osfos", GREEN);
  print(":~", BLUE);
  print("$ ", FG);
}

void help() {
  kernel_printf("Available commands:\n");
  kernel_printf("  help                  Show this list\n");
  kernel_printf("  clear                 Clear the terminal\n");
  kernel_printf("  echo <text>           Print text\n");
  kernel_printf("  about                 Kernel shell information\n");
  kernel_printf("  mem                   Memory-manager test commands\n");
  kernel_printf(
      "  heap-test [bytes]     Allocate/free a heap block (default 64)\n");
  kernel_printf(
      "  pmm-test [pages]      Allocate/free physical pages (default 1)\n");
  kernel_printf("  disk-init             Initialize LazIOfs disk\n");
  kernel_printf("  disk-check <file>     Check if a file exists on disk and "
                "print info\n");
  kernel_printf("  spong                 Play Shitty Pong\n");
  kernel_printf("  doom1                 Launch Doom I\n");
  kernel_printf("  doom2                 Launch Doom II\n");
  kernel_printf("  tetris                The Tetrominoes\n");
  kernel_printf("  breakout              Play Breakout\n");
  kernel_printf("  gtest                 Run Graphic test app suite\n");
  kernel_printf("  stress                Run the stress test\n");
  kernel_printf("  shitcell              Run The Shitty Excel spreadsheet\n");
  kernel_printf("  pain                  Run THE PAIN PNG Image Viewer and "
                "Paint shits\n");
  kernel_printf("  calc                  Use your brain, a pen and a paper "
                "before this\n");
  kernel_printf("  exit                  Explain how to leave the shell\n");
}

void run_heap_test(const char *args) {
  size_t bytes = 64;
  if (*args && (!parse_size(args, bytes) || bytes > 4u * 1024u * 1024u)) {
    kernel_printf("usage: heap-test [bytes], range: 1..4194304\n");
    return;
  }
  void *block = kmalloc(bytes);
  if (!block) {
    kernel_printf("heap-test: allocation failed\n");
    return;
  }
  kfree(block);
  kernel_printf("heap-test: allocated and freed %u bytes\n",
                static_cast<unsigned int>(bytes));
}

void run_pmm_test(const char *args) {
  size_t pages = 1;
  if (*args && (!parse_size(args, pages) || pages > 1024)) {
    kernel_printf("usage: pmm-test [pages], range: 1..1024\n");
    return;
  }
  void *block = Kernel::Memory::g_pmm.allocate_page_block(pages);
  if (!block) {
    kernel_printf("pmm-test: page allocation failed\n");
    return;
  }
  Kernel::Memory::g_pmm.free_page_block(block, pages);
  kernel_printf("pmm-test: allocated and freed %u page(s)\n",
                static_cast<unsigned int>(pages));
}

void run_disk_init(const char *) {
  Kernel::FS::VFS::init();
  if (!Kernel::FS::VFS::is_mounted()) {
    kernel_printf("disk-init: VFS mount failed.\n");
    return;
  }
  kernel_printf("disk-init: VFS mounted successfully.\n");
}

void run_disk_check(const char *args) {
  while (*args == ' ')
    args++;
  if (!*args) {
    kernel_printf("usage: disk-check <filename>\n");
    return;
  }
  Kernel::FS::VFSFile file;
  if (Kernel::FS::VFS::find_file(args, &file)) {
    kernel_printf("File found: %s\n  Size: %u bytes\n  Start LBA: %u\n",
                  file.name, file.size, file.start_lba);
  } else {
    kernel_printf("disk-check: file '%s' not found on disk.\n", args);
  }
}

void run_ls(const char *) {
  if (!Kernel::FS::VFS::is_mounted()) {
    kernel_printf("ls: VFS not mounted\n");
    return;
  }
  kernel_printf("TYPE   SIZE     PATH\n");
  for (uint32_t i = 0; i < FS::VFS_MAX_ENTRIES; ++i) {
    Kernel::FS::VFSFile file;
    if (Kernel::FS::VFS::get_file_at(i, &file) && file.exists) {
      kernel_printf("%s  %u     %s\n", file.is_directory ? "DIR " : "FILE",
                    file.size, file.name);
    }
  }
}

void run_cat(const char *args) {
  while (*args == ' ')
    args++;
  if (!*args) {
    kernel_printf("usage: cat <filename>\n");
    return;
  }
  void *stream = Kernel::FS::VFS::vfs_open(args, "r");
  if (!stream) {
    kernel_printf("cat: %s: No such file or directory\n", args);
    return;
  }
  char buf[128];
  size_t n;
  while ((n = Kernel::FS::VFS::vfs_read(buf, 1, sizeof(buf) - 1, stream)) > 0) {
    buf[n] = '\0';
    kernel_printf("%s", buf);
  }
  kernel_printf("\n");
  Kernel::FS::VFS::vfs_close(stream);
}

void run_xhci_init(const char *) {
  kernel_printf("Searching PCI bus for xHCI controller (0x0C0330)...\n");
  bool found = false;
  for (uint16_t bus = 0; bus < PCI_MAX_BUSES && !found; ++bus) {
    for (uint8_t slot = 0; slot < PCI_MAX_SLOTS && !found; ++slot) {
      for (uint8_t func = 0; func < PCI_MAX_FUNCTIONS && !found; ++func) {
        uint32_t id = pci_read_config(bus, slot, func, 0x00);
        if ((uint16_t)id == 0xFFFF || (uint16_t)id == 0x0000)
          continue;
        uint32_t cls = pci_read_config(bus, slot, func, 0x08);
        if (((cls >> 24) & 0xFF) == 0x0C && ((cls >> 16) & 0xFF) == 0x03 &&
            ((cls >> 8) & 0xFF) == 0x30) {
          found = true;
          kernel_printf("Found xHCI at %02X:%02X.%u\n", bus, slot, func);
          uint32_t cmd = pci_read_config(bus, slot, func, 0x04);
          pci_write_config(bus, slot, func, 0x04, cmd | (1 << 2));
          uint64_t mmio_base =
              ((uint64_t)pci_read_config(bus, slot, func, 0x14) << 32) |
              (pci_read_config(bus, slot, func, 0x10) & 0xFFFFFFF0u);
          kernel_printf("xHCI MMIO Base: 0x%lX\n", mmio_base);
          Kernel::Drivers::USB::XHCIController xhci(mmio_base);
          kernel_printf(xhci.initialize() ? "xHCI initialization PASSED!\n"
                                          : "xHCI initialization FAILED!\n");
        }
      }
    }
  }
  if (!found)
    kernel_printf("xhci-init: No xHCI controller found on PCI bus.\n");
}

void reset_terminal_view() {
  term_vfb.clear(BG);
  draw_header();
  redraw_terminal();
  term_vfb.flush();
}

void execute(char *line) {
  while (*line == ' ')
    ++line;
  if (!*line)
    return;

  char *args = line;
  while (*args && *args != ' ')
    ++args;
  if (*args) {
    *args++ = '\0';
    while (*args == ' ')
      ++args;
  }

  if (equals(line, "help"))
    help();
  else if (equals(line, "clear")) {
    term_vfb.clear(BG);
    draw_header();
    for (size_t r = 0; r < rows; ++r)
      clear_row(r);
    row = col = 0;
    term_vfb.flush();
  } else if (equals(line, "echo"))
    kernel_printf("%s\n", args);
  else if (equals(line, "about"))
    kernel_printf("OSFOS kernel shell\nFramebuffer text UI with Virtual "
                  "Framebuffer abstraction.\n");
  else if (equals(line, "mem"))
    kernel_printf(
        "Memory APIs available in this build:\n  KernelHeap::kmalloc/kfree\n  "
        "PMM::allocate_page_block/free_page_block\n");
  else if (equals(line, "heap-test"))
    run_heap_test(args);
  else if (equals(line, "pmm-test"))
    run_pmm_test(args);
  else if (equals(line, "mema")) {
    if (!launch_app("Memoery Analyzer", Apps::memory_analyzer_run))
      kernel_printf("Could not launch graphics test.\n");
  } else if (equals(line, "disk-init"))
    run_disk_init(args);
  else if (equals(line, "disk-check"))
    run_disk_check(args);
  else if (equals(line, "ls"))
    run_ls(args);
  else if (equals(line, "cat"))
    run_cat(args);
  else if (equals(line, "exit"))
    kernel_printf("The kernel shell runs in ring 0; there is no userspace "
                  "process to exit.\n");
  else if (equals(line, "spong")) {
    if (!launch_app("Spong", run_spong))
      kernel_printf("Could not launch Spong.\n");
  } else if (equals(line, "breakout")) {
    if (!launch_app("Breakout", run_breakout))
      kernel_printf("Could not launch Breakout.\n");
  } else if (equals(line, "doom1")) {
    if (!launch_app("Doom I", run_doom1))
      kernel_printf("Could not launch Doom I.\n");
  } else if (equals(line, "doom2")) {
    if (!launch_app("Doom II", run_doom2))
      kernel_printf("Could not launch Doom II.\n");
  } else if (equals(line, "tetris")) {
    if (!launch_app("Tetris", run_tetris))
      kernel_printf("Could not launch Tetris.\n");
  } else if (equals(line, "gtest")) {
    if (!launch_app("Graphics test", run_graphic_test))
      kernel_printf("Could not launch graphics test.\n");
  } else if (equals(line, "stress")) {
    if (!launch_app("Stress test", run_stress_test))
      kernel_printf("Could not launch stress test.\n");
  } else if (equals(line, "shitcell")) {
    if (!launch_app("Shitcell", run_shitty_excel))
      kernel_printf("Could not launch Shitcell.\n");
  } else if (equals(line, "pain")) {
    if (!launch_app("Pain", run_pain))
      kernel_printf("Could not launch Pain.\n");
  } else if (equals(line, "calc")) {
    if (!launch_app("Calculator", run_calc))
      kernel_printf("Could not launch calculator.\n");
  } else
    kernel_printf("command not found: %s\nType 'help' to list commands.\n",
                  line);
}

void initialize_screen() {
  screen_w = Kernel::Drivers::Framebuffer::width();
  screen_h = Kernel::Drivers::Framebuffer::height();
  const bool has_window = Kernel::Graphics::WindowManager::has_window(
      Kernel::Scheduler::current_thread_id());
  if (has_window) {
    if (!term_vfb.open_window(screen_w > 48 ? screen_w - 48 : screen_w,
                              screen_h > 100 ? screen_h - 100 : screen_h,
                              CELL_W, TOP_H + LINE_H))
      return;
    screen_w = term_vfb.width();
    screen_h = term_vfb.height();
  } else if (!term_vfb.init(0, 0, screen_w, screen_h))
    return;
  if (screen_w == 0 || screen_h <= TOP_H)
    return;

  cols = screen_w / CELL_W;
  rows = (screen_h - TOP_H) / LINE_H;
  if (cols > MAX_COLS)
    cols = MAX_COLS;
  if (rows > MAX_ROWS)
    rows = MAX_ROWS;
  if (cols == 0 || rows == 0)
    return;

  memset(screen, ' ', sizeof(screen));
  reset_terminal_view();

  detect_pci_devices();
  run_xhci_init("1");
  run_vector_benchmark();
  kernel_printf("OSFOS kernel shell ready.\nType 'help' to get started.\n");
}
} // namespace

void run() {
  if (!Kernel::Drivers::Framebuffer::valid())
    return;
  Kernel::Graphics::WindowManager::register_window(
      Kernel::Scheduler::current_thread_id(), "Shell", true);
  initialize_screen();
  if (!cols || !rows)
    return;

  char input[INPUT_SIZE];
  size_t length = 0;
  prompt();

  for (;;) {
    uint32_t cx = col * CELL_W;
    uint32_t cy = TOP_H + row * LINE_H;
    for (uint32_t x = 0; x < CELL_W; ++x)
      if (cx + x < screen_w && cy + CELL_H - 1 < screen_h)
        term_vfb.put_pixel(cx + x, cy + CELL_H - 1, BLUE);
    term_vfb.flush();

    if (!term_vfb.has_focus()) {
      Kernel::Scheduler::thread_sleep_ms(4);
      continue;
    }
    char ch = Kernel::Drivers::Keyboard::getchar();
    if (col < cols && row < rows)
      draw_glyph(cx, cy, screen[row][col], screen_fg[row][col]);
    term_vfb.flush();

    if (ch == '\r' || ch == '\n') {
      put_char('\n', FG);
      input[length] = '\0';
      execute(input);
      length = 0;
      prompt();
    } else if (ch == '\b' || static_cast<unsigned char>(ch) == 127) {
      if (length) {
        --length;
        put_char('\b', FG);
      }
    } else if (ch >= 32 && ch <= 126 && length + 1 < sizeof(input)) {
      input[length++] = ch;
      put_char(ch, FG);
    }
  }
}

} // namespace Kernel::Shell

extern "C" {
void kernel_printf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  while (*fmt) {
    if (*fmt != '%') {
      Kernel::Shell::put_char(*fmt++, Kernel::Shell::FG);
      continue;
    }

    ++fmt;
    if (!*fmt)
      break;

    char pad_char = (*fmt == '0') ? (++fmt, '0') : ' ';
    int width = 0;
    while (*fmt >= '0' && *fmt <= '9')
      width = width * 10 + (*fmt++ - '0');

    bool is_long = false, is_long_long = false;
    if (*fmt == 'l') {
      ++fmt;
      if (*fmt == 'l') {
        is_long_long = true;
        ++fmt;
      } else
        is_long = true;
    }

    if (!*fmt)
      break;

    switch (*fmt) {
    case 's': {
      const char *s = va_arg(args, const char *);
      Kernel::Shell::print(s ? s : "(null)", Kernel::Shell::FG);
      break;
    }
    case 'd':
    case 'i': {
      long long val = is_long_long
                          ? va_arg(args, long long)
                          : (is_long ? va_arg(args, long) : va_arg(args, int));
      Kernel::Shell::print_int(val, 10, false, true, width, pad_char,
                               Kernel::Shell::FG);
      break;
    }
    case 'u':
    case 'x':
    case 'X': {
      int base = (*fmt == 'u') ? 10 : 16;
      bool uppercase = (*fmt == 'X');
      unsigned long long val = is_long_long
                                   ? va_arg(args, unsigned long long)
                                   : (is_long ? va_arg(args, unsigned long)
                                              : va_arg(args, unsigned int));
      Kernel::Shell::print_int(static_cast<long long>(val), base, uppercase,
                               false, width, pad_char, Kernel::Shell::FG);
      break;
    }
    case 'p': {
      void *ptr = va_arg(args, void *);
      Kernel::Shell::print("0x", Kernel::Shell::FG);
      Kernel::Shell::print_int(reinterpret_cast<unsigned long long>(ptr), 16,
                               true, false, 8, '0', Kernel::Shell::FG);
      break;
    }
    case 'c':
      Kernel::Shell::put_char(static_cast<char>(va_arg(args, int)),
                              Kernel::Shell::FG);
      break;
    case '%':
      Kernel::Shell::put_char('%', Kernel::Shell::FG);
      break;
    default:
      Kernel::Shell::put_char('%', Kernel::Shell::FG);
      Kernel::Shell::put_char(*fmt, Kernel::Shell::FG);
      break;
    }
    ++fmt;
  }
  va_end(args);
}
}
