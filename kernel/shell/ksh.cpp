#include <kernel/debug/debug.h>
#include <kernel/drivers/ata.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/net/device.h>
#include <kernel/drivers/pci.h>
#include <kernel/drivers/usb/xhci.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/open_font.h>
#include <kernel/lib/appcommon.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/pmm.h>
#include <kernel/net/smoltcp_bridge.h>
#include <kernel/programs/terminal.h>
#include <kernel/scheduler.h>
#include <kernel/shell/shell.h>
#include <kernel/smp.h>
#include <stdlib.h>
#include <string.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace Kernel::Debug {
struct KernelDebugInfo {
  size_t reclaimed_init_bytes;
  size_t reclaimed_init_pages;
  bool init_reclaimed;
};
extern KernelDebugInfo g_kernel_debug_info;
} // namespace Kernel::Debug

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
namespace Pacman {
void run();
}
} // namespace Kernel::Games

namespace Kernel::Apps {
void graphic_test_run();
void stress_test_run();
void benchmark_run();
void shitty_excel_run();
void shitpad_run();
void memory_analyzer_run();
void task_manager_run();
void font_viewer_run();
void file_viewer_run();
namespace Pain {
void run();
}
namespace Calc {
void run();
}
namespace YouTube {
void run();
}
namespace Cube3D {
void run();
}
} // namespace Kernel::Apps

namespace Kernel::Shell {

// --- Constants & Theme ---
constexpr uint32_t FG = 0xF4D6CD;
constexpr uint32_t BLUE = 0xFAB489;
constexpr uint32_t GREEN = 0xA1E3A6;
constexpr size_t INPUT_SIZE = 256;

namespace {
struct AppLaunch {
  void (*entry)();
  char title[32];
};

void app_thread_entry(void *argument) {
  auto *launch = static_cast<AppLaunch *>(argument);
  void (*entry)() = launch ? launch->entry : nullptr;
  char title[32]{};
  if (launch != nullptr) {
    size_t index = 0;
    while (index + 1 < sizeof(title) && (launch->title[index] != 0)) {
      title[index] = launch->title[index];
      ++index;
    }
    title[index] = '\0';
  }
  Kernel::Scheduler::mark_current_thread_as_application(title);
  if (launch != nullptr)
    free(launch);
  while (!Kernel::Scheduler::should_stop() &&
         !Kernel::Graphics::WindowManager::has_window(
             Kernel::Scheduler::current_thread_id()))
    Kernel::Scheduler::thread_sleep_ms(1);
  if ((entry != nullptr) && !Kernel::Scheduler::should_stop())
    entry();
  Kernel::AppCommon::exit();
}

bool launch_app(const char *title, void (*entry)()) {
  auto *launch = static_cast<AppLaunch *>(malloc(sizeof(AppLaunch)));
  if (!launch)
    return false;
  launch->entry = entry;
  size_t title_index = 0;
  while (title_index + 1 < sizeof(launch->title) && title[title_index]) {
    launch->title[title_index] = title[title_index];
    ++title_index;
  }
  launch->title[title_index] = '\0';
  int id = Kernel::Scheduler::thread_create(app_thread_entry, launch);
  if (id < 0) {
    free(launch);
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
void run_pacman() { Kernel::Games::Pacman::run(); }
void run_graphic_test() { Kernel::Apps::graphic_test_run(); }
void run_stress_test() { Kernel::Apps::stress_test_run(); }
void run_benchmark() { Kernel::Apps::benchmark_run(); }
void run_shitty_excel() { Kernel::Apps::shitty_excel_run(); }
void run_shitpad() { Kernel::Apps::shitpad_run(); }
void run_c3d() { Kernel::Apps::Cube3D::run(); }
void run_pain() { Kernel::Apps::Pain::run(); }
void run_calc() { Kernel::Apps::Calc::run(); }
void run_youtube() { Kernel::Apps::YouTube::run(); }

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

void put_char(char ch, uint32_t color) {
  Kernel::Programs::Terminal::write_char(ch, color);
}

void print(const char *s, uint32_t color) {
  while (*s)
    put_char(*s++, color);
  Kernel::Programs::Terminal::flush();
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
void show_debug_info() {
  const auto memory = Kernel::Memory::g_kheap.get_system_memory_info();
  kprintf("Debug status: heap %u/%u bytes used; PMM %u/%u bytes used\n",
          static_cast<unsigned>(memory.heap_used_size),
          static_cast<unsigned>(memory.heap_total_size),
          static_cast<unsigned>(memory.pmm_used_memory),
          static_cast<unsigned>(memory.pmm_total_memory));
  kprintf("Heap integrity: %s; recent log entries:\n",
          Kernel::Memory::g_kheap.validate() ? "OK" : "FAILED");
  const size_t count = Kernel::Debug::log_count();
  for (size_t i = 0; i < count; ++i) {
    Kernel::Debug::LogEntry entry{};
    if (Kernel::Debug::read_log(i, &entry))
      kprintf("  [%u] %s\n", static_cast<unsigned>(i), entry.message);
  }
}

void prompt() {
  print("root@osfos", GREEN);
  print(":~", BLUE);
  print("$ ", FG);
}

void help() {
  kprintf("Available commands:\n");
  kprintf("  help                  Show this list\n");
  kprintf("  clear                 Clear the terminal\n");
  kprintf("  echo <text>           Print text\n");
  kprintf("  ping <host>           Send ICMP echo requests\n");
  kprintf("  netup                 Acquire network settings with DHCP\n");
  kprintf("  netinfo               Show network interface settings\n");
  kprintf("  fetch <url>           Fetch a bounded HTTP or verified "
          "HTTPS response\n");
  kprintf("  youtube               Search YouTube and show thumbnails\n");
  kprintf("  tcp-echo <host> <port> <text>  Send text to a TCP echo service\n");
  kprintf("  about                 Kernel shell information\n");
  kprintf(
      "  debug                 Show allocator health and recent kernel log\n");
  kprintf("  smpinfo               Show detected and online CPUs\n");
  kprintf("  mem                   Memory-manager test commands\n");
  kprintf("  taskman               List running apps and task stats\n");
  kprintf("  heap-test [bytes]     Allocate/free a heap block (default 64)\n");
  kprintf("  pmm-test [pages]      Allocate/free physical pages (default 1)\n");
  kprintf("  disk-init             Initialize LazIOfs disk\n");
  kprintf(
      "  xhci-init             Probe and initialize a USB xHCI controller\n");
  kprintf("  disk-check <file>     Check if a file exists on disk and "
          "print info\n");
  kprintf("  spong                 Play Shitty Pong\n");
  kprintf("  doom1                 Launch Doom I\n");
  kprintf("  doom2                 Launch Doom II\n");
  kprintf("  tetris                The Tetrominoes\n");
  kprintf("  pacman                Play Pac-Man\n");
  kprintf("  breakout              Play Breakout\n");
  kprintf("  gtest                 Run Graphic test app suite\n");
  kprintf("  stress                Run the stress test\n");
  kprintf("  bench                 Run the system benchmark suite\n");
  kprintf("  shitcell              Run The Shitty Excel spreadsheet\n");
  kprintf("  shitpad               Run The Shitpad Editor\n");
  kprintf("  pain                  Run THE PAIN PNG Image Viewer and "
          "Paint shits\n");
  kprintf("  c3d                   Run the 3D showoff\n");
  kprintf("  calc                  Use your brain, a pen and a paper "
          "before this\n");
  kprintf("  fontview              Open the TrueType font viewer/check app\n");
  kprintf("  fileview              Browse and read files on the disk\n");
  kprintf("  fontcheck             List available TTF files and font status\n");
  kprintf("  exit                  Explain how to leave the shell\n");
}

void run_heap_test(const char *args) {
  size_t bytes = 64;
  if (*args && (!parse_size(args, bytes) || bytes > 4u * 1024u * 1024u)) {
    kprintf("usage: heap-test [bytes], range: 1..4194304\n");
    return;
  }
  void *block = malloc(bytes);
  if (!block) {
    kprintf("heap-test: allocation failed\n");
    return;
  }
  free(block);
  kprintf("heap-test: allocated and freed %u bytes\n",
          static_cast<unsigned int>(bytes));
}

void run_pmm_test(const char *args) {
  size_t pages = 1;
  if (*args && (!parse_size(args, pages) || pages > 1024)) {
    kprintf("usage: pmm-test [pages], range: 1..1024\n");
    return;
  }
  void *block = Kernel::Memory::g_pmm.allocate_page_block(pages);
  if (!block) {
    kprintf("pmm-test: page allocation failed\n");
    return;
  }
  Kernel::Memory::g_pmm.free_page_block(block, pages);
  kprintf("pmm-test: allocated and freed %u page(s)\n",
          static_cast<unsigned int>(pages));
}

void run_disk_init(const char *) {
  Kernel::FS::VFS::init();
  if (!Kernel::FS::VFS::is_mounted()) {
    kprintf("disk-init: VFS mount failed.\n");
    return;
  }
  kprintf("disk-init: VFS mounted successfully.\n");
}

void run_disk_check(const char *args) {
  while (*args == ' ')
    args++;
  if (!*args) {
    kprintf("usage: disk-check <filename>\n");
    return;
  }
  Kernel::FS::VFSFile file;
  if (Kernel::FS::VFS::find_file(args, &file)) {
    kprintf("File found: %s\n  Size: %u bytes\n  Start LBA: %u\n", file.name,
            file.size, file.start_lba);
  } else {
    kprintf("disk-check: file '%s' not found on disk.\n", args);
  }
}

void run_ls(const char *) {
  if (!Kernel::FS::VFS::is_mounted()) {
    kprintf("ls: VFS not mounted\n");
    return;
  }
  kprintf("TYPE   SIZE     PATH\n");
  for (uint32_t i = 0; i < FS::VFS_MAX_INODES; ++i) {
    Kernel::FS::VFSFile file;
    if (Kernel::FS::VFS::get_file_at(i, &file) && file.exists) {
      kprintf("%s  %u     %s\n", file.is_directory ? "DIR " : "FILE", file.size,
              file.name);
    }
  }
}

void run_cat(const char *args) {
  while (*args == ' ')
    args++;
  if (!*args) {
    kprintf("usage: cat <filename>\n");
    return;
  }
  void *stream = Kernel::FS::VFS::vfs_open(args, "r");
  if (!stream) {
    kprintf("cat: %s: No such file or directory\n", args);
    return;
  }
  char buf[128];
  size_t n;
  while ((n = Kernel::FS::VFS::vfs_read(buf, 1, sizeof(buf) - 1, stream)) > 0) {
    buf[n] = '\0';
    kprintf("%s", buf);
  }
  kprintf("\n");
  Kernel::FS::VFS::vfs_close(stream);
}

void run_xhci_init(const char *) {
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
          uint32_t cmd = pci_read_config(bus, slot, func, 0x04);
          pci_write_config(bus, slot, func, 0x04, cmd | (1 << 2));
          uint64_t mmio_base =
              ((uint64_t)pci_read_config(bus, slot, func, 0x14) << 32) |
              (pci_read_config(bus, slot, func, 0x10) & 0xFFFFFFF0u);
          Kernel::Drivers::USB::XHCIController xhci(mmio_base);
          if (!xhci.initialize())
            kprintf("xhci-init: controller initialization failed\n");
        }
      }
    }
  }
}

void print_ipv4(uint32_t address) {
  kprintf("%u.%u.%u.%u", (address >> 24) & 255, (address >> 16) & 255,
          (address >> 8) & 255, address & 255);
}

void run_netup(const char *) {
  const int result = Kernel::Net::configure(8000);
  if (result != 0) {
    kprintf("netup: DHCP failed (%d)\n", result);
    return;
  }
  kprintf("Network configured.\n");
}

void run_netinfo(const char *) {
  Kernel::Net::Info info{};
  if (Kernel::Net::get_info(&info) != 0) {
    kprintf("netinfo: network stack unavailable\n");
    return;
  }
  const auto *device = Kernel::Drivers::Net::active_device();
  const char *driver = "unknown";
  if (device && device->name) {
    if (equals(device->name, "rtl8168"))
      driver = "realtek";
    else
      driver = device->name;
  }
  kprintf("Driver: %s\n", driver);
  kprintf("MAC %02X:%02X:%02X:%02X:%02X:%02X\n", info.mac[0], info.mac[1],
          info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
  if (!info.configured) {
    kprintf("IPv4: not configured (run netup)\n");
    return;
  }
  kprintf("IPv4: ");
  print_ipv4(info.ipv4);
  kprintf("/%u\nGateway: ", info.prefix_len);
  print_ipv4(info.gateway);
  kprintf("\nDNS: ");
  print_ipv4(info.dns);
  kprintf("\n");
}

void run_ping(const char *host) {
  if (!*host) {
    kprintf("usage: ping <host>\n");
    return;
  }
  Kernel::Net::PingStats stats{};
  const int result = Kernel::Net::ping(host, &stats);
  if (result != 0) {
    kprintf("ping: failed (%d)\n", result);
    return;
  }
  for (uint32_t sequence = 0; sequence < 4; ++sequence) {
    if (stats.received_mask & (1u << sequence))
      kprintf("reply from %s: seq=%u time=%u ms\n", host, sequence + 1,
              stats.rtt_ms[sequence]);
    else
      kprintf("request timed out (seq=%u)\n", sequence + 1);
  }
  kprintf("%u/%u replies received\n", stats.received, stats.sent);
}

void run_tcp_echo(char *args) {
  char *host = args;
  while (*args && *args != ' ')
    ++args;
  if (!*args) {
    kprintf("usage: tcp-echo <host> <port> <text>\n");
    return;
  }
  *args++ = '\0';
  while (*args == ' ')
    ++args;
  char *port_text = args;
  while (*args && *args != ' ')
    ++args;
  if (!*args) {
    kprintf("usage: tcp-echo <host> <port> <text>\n");
    return;
  }
  *args++ = '\0';
  while (*args == ' ')
    ++args;
  size_t port = 0;
  if (!*args || !parse_size(port_text, port) || port > 65535) {
    kprintf("usage: tcp-echo <host> <port> <text>\n");
    return;
  }

  char response[512]{};
  const int length =
      Kernel::Net::tcp_echo(host, static_cast<uint16_t>(port), args, response,
                            sizeof(response) - 1, 5000);
  if (length < 0) {
    kprintf("tcp-echo: failed (%d)\n", length);
    return;
  }
  response[length] = '\0';
  kprintf("received %d bytes: %s\n", length, response);
}

void run_fontcheck(const char *) {
  kprintf("TrueType font availability:\n");

  Kernel::Graphics::TrueTypeFont embedded_font;
  const bool embedded_ok =
      Kernel::Graphics::OpenFont::load_embedded(embedded_font);
  kprintf("  embedded fallback: %s\n", embedded_ok ? "READY" : "MISSING");

  if (!Kernel::FS::VFS::is_mounted()) {
    kprintf("  VFS font directory: unavailable\n");
    return;
  }

  bool found_any = false;
  for (uint32_t i = 0; i < Kernel::FS::VFS_MAX_INODES; ++i) {
    Kernel::FS::VFSFile file{};
    if (!Kernel::FS::VFS::get_file_at(i, &file) || !file.exists ||
        file.is_directory)
      continue;

    const size_t name_len = strlen(file.name);
    if (name_len < 4)
      continue;

    const char *ext = file.name + name_len - 4;
    const bool is_ttf = (ext[0] == '.' && ((ext[1] == 't' || ext[1] == 'T') &&
                                           (ext[2] == 't' || ext[2] == 'T') &&
                                           (ext[3] == 'f' || ext[3] == 'F')));
    if (!is_ttf)
      continue;

    found_any = true;
    Kernel::Graphics::TrueTypeFont font;
    const bool ok = Kernel::Graphics::OpenFont::load(font, file.name);
    kprintf("  %s: %s\n", file.name, ok ? "loadable" : "unavailable");
  }

  if (!found_any)
    kprintf("  no .ttf files found in the mounted VFS\n");
}

void run_fetch(const char *url) {
  if (!*url) {
    kprintf("usage: fetch <http[s]://host[:port]/path>\n");
    return;
  }
  char response[2048]{};
  const int length = Kernel::Net::fetch(url, response, sizeof(response));
  if (length == -8) {
    kprintf("fetch: invalid URL; use http:// or https://\n");
    return;
  }
  if (length == -4) {
    kprintf("fetch: hardware RDRAND entropy unavailable\n");
    return;
  }
  if (length == -5) {
    kprintf("fetch: RTC time unavailable; HTTPS certificate checks refused\n");
    return;
  }
  if (length == -6) {
    kprintf("fetch: HTTPS handshake or certificate verification failed\n");
    return;
  }
  if (length <= -1000) {
    const int detail = -1000 - length;
    if (detail >= 1100 && detail < 1400) {
      const int failure = (detail - 1000) / 100;
      const int tls_error = (detail - 1000) % 100;
      if (failure == 1)
        kprintf("fetch: TCP receive timed out during TLS (BearSSL %d)\n",
                tls_error);
      else if (failure == 2)
        kprintf("fetch: peer closed TCP during TLS (BearSSL %d)\n", tls_error);
      else
        kprintf("fetch: TCP send failed during TLS (BearSSL %d)\n", tls_error);
      return;
    }
    const int tls_error = detail;
    switch (tls_error) {
    case 3:
      kprintf("fetch: peer does not support TLS 1.2\n");
      break;
    case 8:
      kprintf("fetch: BearSSL could not seed its random generator\n");
      break;
    case 31:
      kprintf("fetch: TCP transport failed during TLS\n");
      break;
    case 53:
      kprintf("fetch: certificate time was not available\n");
      break;
    case 54:
      kprintf("fetch: server certificate is expired or not yet valid\n");
      break;
    case 56:
      kprintf("fetch: server certificate hostname mismatch\n");
      break;
    case 62:
      kprintf("fetch: server certificate chain is not trusted\n");
      break;
    default:
      kprintf("fetch: BearSSL error %d\n", tls_error);
      break;
    }
    return;
  }
  if (length < 0) {
    kprintf("fetch: request failed (%d)\n", length);
    return;
  }
  kprintf("%s", response);
  if (length == static_cast<int>(sizeof(response) - 1))
    kprintf("\n[response truncated at %u bytes]\n",
            static_cast<unsigned int>(sizeof(response) - 1));
  else
    kprintf("\n");
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
    Kernel::Programs::Terminal::clear();
    Kernel::Programs::Terminal::flush();
  } else if (equals(line, "echo"))
    kprintf("%s\n", args);
  else if (equals(line, "ping"))
    run_ping(args);
  else if (equals(line, "netup"))
    run_netup(args);
  else if (equals(line, "netinfo"))
    run_netinfo(args);
  else if (equals(line, "tcp-echo"))
    run_tcp_echo(args);
  else if (equals(line, "fetch"))
    run_fetch(args);
  else if (equals(line, "youtube")) {
    if (!launch_app("YouTube", run_youtube))
      kprintf("Could not launch YouTube.\n");
  } else if (equals(line, "fontcheck"))
    run_fontcheck(args);
  else if (equals(line, "fontview")) {
    if (!launch_app("Font Viewer", Apps::font_viewer_run))
      kprintf("Could not launch Font Viewer.\n");
  } else if (equals(line, "fileview")) {
    if (!launch_app("File Viewer", Apps::file_viewer_run))
      kprintf("Could not launch File Viewer.\n");
  } else if (equals(line, "about"))
    kprintf("OSFOS kernel shell\nFramebuffer text UI with Virtual "
            "Framebuffer abstraction.\n");
  else if (equals(line, "smpinfo"))
    kprintf("Online processors: %u\n",
            static_cast<unsigned>(Kernel::SMP::online_cpu_count()));
  else if (equals(line, "debug"))
    show_debug_info();
  else if (equals(line, "mem"))
    kprintf(
        "Memory APIs available in this build:\n  KernelHeap::malloc/free\n  "
        "PMM::allocate_page_block/free_page_block\n");
  else if (equals(line, "taskman")) {
    if (!launch_app("Task Manager", Apps::task_manager_run))
      kprintf("Could not launch Task Manager.\n");
  } else if (equals(line, "heap-test"))
    run_heap_test(args);
  else if (equals(line, "pmm-test"))
    run_pmm_test(args);
  else if (equals(line, "mema")) {
    if (!launch_app("Memoery Analyzer", Apps::memory_analyzer_run))
      kprintf("Could not launch graphics test.\n");
  } else if (equals(line, "disk-init"))
    run_disk_init(args);
  else if (equals(line, "xhci-init"))
    run_xhci_init(args);
  else if (equals(line, "disk-check"))
    run_disk_check(args);
  else if (equals(line, "ls"))
    run_ls(args);
  else if (equals(line, "cat"))
    run_cat(args);
  else if (equals(line, "exit"))
    kprintf("The kernel shell runs in ring 0; there is no userspace "
            "process to exit.\n");
  else if (equals(line, "spong")) {
    if (!launch_app("Spong", run_spong))
      kprintf("Could not launch Spong.\n");
  } else if (equals(line, "breakout")) {
    if (!launch_app("Breakout", run_breakout))
      kprintf("Could not launch Breakout.\n");
  } else if (equals(line, "doom1")) {
    if (!launch_app("Doom I", run_doom1))
      kprintf("Could not launch Doom I.\n");
  } else if (equals(line, "doom2")) {
    if (!launch_app("Doom II", run_doom2))
      kprintf("Could not launch Doom II.\n");
  } else if (equals(line, "tetris")) {
    if (!launch_app("Tetris", run_tetris))
      kprintf("Could not launch Tetris.\n");
  } else if (equals(line, "pacman")) {
    if (!launch_app("Pac-Man", run_pacman))
      kprintf("Could not launch Pac-Man.\n");
  } else if (equals(line, "gtest")) {
    if (!launch_app("Graphics test", run_graphic_test))
      kprintf("Could not launch graphics test.\n");
  } else if (equals(line, "stress")) {
    if (!launch_app("Stress test", run_stress_test))
      kprintf("Could not launch stress test.\n");
  } else if (equals(line, "bench")) {
    if (!launch_app("System benchmark", run_benchmark))
      kprintf("Could not launch system benchmark.\n");
  } else if (equals(line, "shitcell")) {
    if (!launch_app("Shitcell", run_shitty_excel))
      kprintf("Could not launch Shitcell.\n");
  } else if (equals(line, "pain")) {
    if (!launch_app("Pain", run_pain))
      kprintf("Could not launch Pain.\n");
  } else if (equals(line, "calc")) {
    if (!launch_app("Calculator", run_calc))
      kprintf("Could not launch calculator.\n");
  } else if (equals(line, "shitpad")) {
    if (!launch_app("Shitpad Editor", run_shitpad))
      kprintf("Could not launch calculator.\n");
  } else if (equals(line, "c3d")) {
    if (!launch_app("Cube3D", run_c3d))
      kprintf("Could not launch calculator.\n");
  } else
    kprintf("command not found: %s\nType 'help' to list commands.\n", line);
}

} // namespace

void run() {
  Kernel::Graphics::WindowManager::register_window(
      Kernel::Scheduler::current_thread_id(), "Shell", true);
  if (!Kernel::Programs::Terminal::initialize())
    return;

  if (Kernel::Debug::g_kernel_debug_info.init_reclaimed) {
    kprintf("Reclaimed %zu bytes (%zu pages) of init memory.\n",
            Kernel::Debug::g_kernel_debug_info.reclaimed_init_bytes,
            Kernel::Debug::g_kernel_debug_info.reclaimed_init_pages);
  }
  size_t size = 0;
  kprintf("Embedded font size: %u bytes\n", size);
  kprintf("OSFOS kernel shell ready.\nType 'help' to get started.\n");

  char input[INPUT_SIZE];
  size_t length = 0;
  prompt();

  for (;;) {
    const size_t cursor_column = Kernel::Programs::Terminal::cursor_column();
    const size_t cursor_row = Kernel::Programs::Terminal::cursor_row();
    Kernel::Programs::Terminal::show_cursor(cursor_column, cursor_row, BLUE);

    if (!Kernel::Programs::Terminal::has_focus()) {
      Kernel::Scheduler::thread_sleep_ms(4);
      continue;
    }
    char ch = Kernel::Drivers::Keyboard::getchar();
    Kernel::Programs::Terminal::hide_cursor(cursor_column, cursor_row);

    if (ch == '\r' || ch == '\n') {
      put_char('\n', FG);
      input[length] = '\0';
      execute(input);
      Kernel::Programs::Terminal::flush();
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
void kprintf(const char *fmt, ...) {
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

    bool is_long = false, is_long_long = false, is_size = false;
    if (*fmt == 'l') {
      ++fmt;
      if (*fmt == 'l') {
        is_long_long = true;
        ++fmt;
      } else
        is_long = true;
    } else if (*fmt == 'z') {
      is_size = true;
      ++fmt;
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
      long long val = is_size ? static_cast<long long>(va_arg(args, size_t))
                              : (is_long_long ? va_arg(args, long long)
                                              : (is_long ? va_arg(args, long)
                                                         : va_arg(args, int)));
      Kernel::Shell::print_int(val, 10, false, true, width, pad_char,
                               Kernel::Shell::FG);
      break;
    }
    case 'u':
    case 'x':
    case 'X': {
      int base = (*fmt == 'u') ? 10 : 16;
      bool uppercase = (*fmt == 'X');
      unsigned long long val =
          is_size ? static_cast<unsigned long long>(va_arg(args, size_t))
                  : (is_long_long ? va_arg(args, unsigned long long)
                                  : (is_long ? va_arg(args, unsigned long)
                                             : va_arg(args, unsigned int)));
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
  Kernel::Programs::Terminal::flush();
}
}
