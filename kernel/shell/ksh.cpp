#include <kernel/debug/debug.h>
#include <kernel/drivers/net/device.h>
#include <kernel/drivers/pci.h>
#include <kernel/drivers/usb/xhci.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/open_font.h>
#include <kernel/lib/appcommon.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/pmm.h>
#include <kernel/net/smoltcp.h>
#include <kernel/programs/terminal.h>
#include <kernel/scheduler.h>
#include <kernel/shell/shell.h>
#include <kernel/smp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

namespace {
// --- Command Implementations ---
void show_debug_info() {
  const auto memory = Kernel::Memory::g_kheap.get_system_memory_info();
  printf("Debug status: heap %u/%u bytes used; PMM %u/%u bytes used\n",
         static_cast<unsigned>(memory.heap_used_size),
         static_cast<unsigned>(memory.heap_total_size),
         static_cast<unsigned>(memory.pmm_used_memory),
         static_cast<unsigned>(memory.pmm_total_memory));
  printf("Heap integrity: %s; recent log entries:\n",
         Kernel::Memory::g_kheap.validate() ? "OK" : "FAILED");
  const size_t count = Kernel::Debug::log_count();
  for (size_t i = 0; i < count; ++i) {
    Kernel::Debug::LogEntry entry{};
    if (Kernel::Debug::read_log(i, &entry))
      printf("  [%u] %s\n", static_cast<unsigned>(i), entry.message);
  }
}

void prompt() { printf("root@osfos:~$ "); }

void help() {
  printf("Available commands:\n");
  printf("  help                  Show this list\n");
  printf("  clear                 Clear the terminal\n");
  printf("  echo <text>           Print text\n");
  printf("  ping <host>           Send ICMP echo requests\n");
  printf("  netup                 Acquire network settings with DHCP\n");
  printf("  netinfo               Show network interface settings\n");
  printf("  fetch <url>           Fetch a bounded HTTP or verified "
         "HTTPS response\n");
  printf("  youtube               Search YouTube and show thumbnails\n");
  printf("  tcp-echo <host> <port> <text>  Send text to a TCP echo service\n");
  printf("  about                 Kernel shell information\n");
  printf(
      "  debug                 Show allocator health and recent kernel log\n");
  printf("  smpinfo               Show detected and online CPUs\n");
  printf("  mem                   Memory-manager test commands\n");
  printf("  taskman               List running apps and task stats\n");
  printf("  heap-test [bytes]     Allocate/free a heap block (default 64)\n");
  printf("  pmm-test [pages]      Allocate/free physical pages (default 1)\n");
  printf("  disk-init             Initialize LazIOfs disk\n");
  printf(
      "  xhci-init             Probe and initialize a USB xHCI controller\n");
  printf("  disk-check <file>     Check if a file exists on disk and "
         "print info\n");
  printf("  spong                 Play Shitty Pong\n");
  printf("  doom1                 Launch Doom I\n");
  printf("  doom2                 Launch Doom II\n");
  printf("  tetris                The Tetrominoes\n");
  printf("  pacman                Play Pac-Man\n");
  printf("  breakout              Play Breakout\n");
  printf("  gtest                 Run Graphic test app suite\n");
  printf("  stress                Run the stress test\n");
  printf("  bench                 Run the system benchmark suite\n");
  printf("  shitcell              Run The Shitty Excel spreadsheet\n");
  printf("  shitpad               Run The Shitpad Editor\n");
  printf("  pain                  Run THE PAIN PNG Image Viewer and "
         "Paint shits\n");
  printf("  c3d                   Run the 3D showoff\n");
  printf("  calc                  Use your brain, a pen and a paper "
         "before this\n");
  printf("  fontview              Open the TrueType font viewer/check app\n");
  printf("  fileview              Browse and read files on the disk\n");
  printf("  fontcheck             List available TTF files and font status\n");
  printf("  exit                  Explain how to leave the shell\n");
}

void run_heap_test(const char *args) {
  size_t bytes = 64;
  if (*args && (!parse_size(args, bytes) || bytes > 4u * 1024u * 1024u)) {
    printf("usage: heap-test [bytes], range: 1..4194304\n");
    return;
  }
  void *block = malloc(bytes);
  if (!block) {
    printf("heap-test: allocation failed\n");
    return;
  }
  free(block);
  printf("heap-test: allocated and freed %u bytes\n",
         static_cast<unsigned int>(bytes));
}

void run_pmm_test(const char *args) {
  size_t pages = 1;
  if (*args && (!parse_size(args, pages) || pages > 1024)) {
    printf("usage: pmm-test [pages], range: 1..1024\n");
    return;
  }
  void *block = Kernel::Memory::g_pmm.allocate_page_block(pages);
  if (!block) {
    printf("pmm-test: page allocation failed\n");
    return;
  }
  Kernel::Memory::g_pmm.free_page_block(block, pages);
  printf("pmm-test: allocated and freed %u page(s)\n",
         static_cast<unsigned int>(pages));
}

void run_disk_init(const char *) {
  Kernel::FS::VFS::init();
  if (!Kernel::FS::VFS::is_mounted()) {
    printf("disk-init: VFS mount failed.\n");
    return;
  }
  printf("disk-init: VFS mounted successfully.\n");
}

void run_disk_check(const char *args) {
  while (*args == ' ')
    args++;
  if (!*args) {
    printf("usage: disk-check <filename>\n");
    return;
  }
  Kernel::FS::VFSFile file;
  if (Kernel::FS::VFS::find_file(args, &file)) {
    printf("File found: %s\n  Size: %u bytes\n  Start LBA: %u\n", file.name,
           file.size, file.start_lba);
  } else {
    printf("disk-check: file '%s' not found on disk.\n", args);
  }
}

void run_ls(const char *) {
  if (!Kernel::FS::VFS::is_mounted()) {
    printf("ls: VFS not mounted\n");
    return;
  }
  printf("TYPE   SIZE     PATH\n");
  for (uint32_t i = 0; i < FS::VFS_MAX_INODES; ++i) {
    Kernel::FS::VFSFile file;
    if (Kernel::FS::VFS::get_file_at(i, &file) && file.exists) {
      printf("%s  %u     %s\n", file.is_directory ? "DIR " : "FILE", file.size,
             file.name);
    }
  }
}

void run_cat(const char *args) {
  while (*args == ' ')
    args++;
  if (!*args) {
    printf("usage: cat <filename>\n");
    return;
  }
  void *stream = Kernel::FS::VFS::vfs_open(args, "r");
  if (!stream) {
    printf("cat: %s: No such file or directory\n", args);
    return;
  }
  char buf[128];
  size_t n;
  while ((n = Kernel::FS::VFS::vfs_read(buf, 1, sizeof(buf) - 1, stream)) > 0) {
    buf[n] = '\0';
    printf("%s", buf);
  }
  printf("\n");
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
            printf("xhci-init: controller initialization failed\n");
        }
      }
    }
  }
}

void print_ipv4(uint32_t address) {
  printf("%u.%u.%u.%u", (address >> 24) & 255, (address >> 16) & 255,
         (address >> 8) & 255, address & 255);
}

void run_netup(const char *) {
  const int result = Kernel::Net::configure(8000);
  if (result != 0) {
    printf("netup: DHCP failed (%d)\n", result);
    return;
  }
  printf("Network configured.\n");
}

void run_netinfo(const char *) {
  Kernel::Net::Info info{};
  if (Kernel::Net::get_info(&info) != 0) {
    printf("netinfo: network stack unavailable\n");
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
  printf("Driver: %s\n", driver);
  printf("MAC %02X:%02X:%02X:%02X:%02X:%02X\n", info.mac[0], info.mac[1],
         info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
  if (!info.configured) {
    printf("IPv4: not configured (run netup)\n");
    return;
  }
  printf("IPv4: ");
  print_ipv4(info.ipv4);
  printf("/%u\nGateway: ", info.prefix_len);
  print_ipv4(info.gateway);
  printf("\nDNS: ");
  print_ipv4(info.dns);
  printf("\n");
}

void run_ping(const char *host) {
  if (!*host) {
    printf("usage: ping <host>\n");
    return;
  }
  Kernel::Net::PingStats stats{};
  const int result = Kernel::Net::ping(host, &stats);
  if (result != 0) {
    printf("ping: failed (%d)\n", result);
    return;
  }
  for (uint32_t sequence = 0; sequence < 4; ++sequence) {
    if (stats.received_mask & (1u << sequence))
      printf("reply from %s: seq=%u time=%u ms\n", host, sequence + 1,
             stats.rtt_ms[sequence]);
    else
      printf("request timed out (seq=%u)\n", sequence + 1);
  }
  printf("%u/%u replies received\n", stats.received, stats.sent);
}

void run_tcp_echo(char *args) {
  char *host = args;
  while (*args && *args != ' ')
    ++args;
  if (!*args) {
    printf("usage: tcp-echo <host> <port> <text>\n");
    return;
  }
  *args++ = '\0';
  while (*args == ' ')
    ++args;
  char *port_text = args;
  while (*args && *args != ' ')
    ++args;
  if (!*args) {
    printf("usage: tcp-echo <host> <port> <text>\n");
    return;
  }
  *args++ = '\0';
  while (*args == ' ')
    ++args;
  size_t port = 0;
  if (!*args || !parse_size(port_text, port) || port > 65535) {
    printf("usage: tcp-echo <host> <port> <text>\n");
    return;
  }

  char response[512]{};
  const int length =
      Kernel::Net::tcp_echo(host, static_cast<uint16_t>(port), args, response,
                            sizeof(response) - 1, 5000);
  if (length < 0) {
    printf("tcp-echo: failed (%d)\n", length);
    return;
  }
  response[length] = '\0';
  printf("received %d bytes: %s\n", length, response);
}

void run_fontcheck(const char *) {
  printf("TrueType font availability:\n");

  Kernel::Graphics::TrueTypeFont embedded_font;
  const bool embedded_ok =
      Kernel::Graphics::OpenFont::load_embedded(embedded_font);
  printf("  embedded fallback: %s\n", embedded_ok ? "READY" : "MISSING");

  if (!Kernel::FS::VFS::is_mounted()) {
    printf("  VFS font directory: unavailable\n");
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
    printf("  %s: %s\n", file.name, ok ? "loadable" : "unavailable");
  }

  if (!found_any)
    printf("  no .ttf files found in the mounted VFS\n");
}

void run_fetch(const char *url) {
  if (!*url) {
    printf("usage: fetch <http[s]://host[:port]/path>\n");
    return;
  }
  char response[2048]{};
  const int length = Kernel::Net::fetch(url, response, sizeof(response));
  if (length == -8) {
    printf("fetch: invalid URL; use http:// or https://\n");
    return;
  }
  if (length == -4) {
    printf("fetch: hardware RDRAND entropy unavailable\n");
    return;
  }
  if (length == -5) {
    printf("fetch: RTC time unavailable; HTTPS certificate checks refused\n");
    return;
  }
  if (length == -6) {
    printf("fetch: HTTPS handshake or certificate verification failed\n");
    return;
  }
  if (length <= -1000) {
    const int detail = -1000 - length;
    if (detail >= 1100 && detail < 1400) {
      const int failure = (detail - 1000) / 100;
      const int tls_error = (detail - 1000) % 100;
      if (failure == 1)
        printf("fetch: TCP receive timed out during TLS (BearSSL %d)\n",
               tls_error);
      else if (failure == 2)
        printf("fetch: peer closed TCP during TLS (BearSSL %d)\n", tls_error);
      else
        printf("fetch: TCP send failed during TLS (BearSSL %d)\n", tls_error);
      return;
    }
    const int tls_error = detail;
    switch (tls_error) {
    case 3:
      printf("fetch: peer does not support TLS 1.2\n");
      break;
    case 8:
      printf("fetch: BearSSL could not seed its random generator\n");
      break;
    case 31:
      printf("fetch: TCP transport failed during TLS\n");
      break;
    case 53:
      printf("fetch: certificate time was not available\n");
      break;
    case 54:
      printf("fetch: server certificate is expired or not yet valid\n");
      break;
    case 56:
      printf("fetch: server certificate hostname mismatch\n");
      break;
    case 62:
      printf("fetch: server certificate chain is not trusted\n");
      break;
    default:
      printf("fetch: BearSSL error %d\n", tls_error);
      break;
    }
    return;
  }
  if (length < 0) {
    printf("fetch: request failed (%d)\n", length);
    return;
  }
  printf("%s", response);
  if (length == static_cast<int>(sizeof(response) - 1))
    printf("\n[response truncated at %u bytes]\n",
           static_cast<unsigned int>(sizeof(response) - 1));
  else
    printf("\n");
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
    printf("\f");
  } else if (equals(line, "echo"))
    printf("%s\n", args);
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
      printf("Could not launch YouTube.\n");
  } else if (equals(line, "fontcheck"))
    run_fontcheck(args);
  else if (equals(line, "fontview")) {
    if (!launch_app("Font Viewer", Apps::font_viewer_run))
      printf("Could not launch Font Viewer.\n");
  } else if (equals(line, "fileview")) {
    if (!launch_app("File Viewer", Apps::file_viewer_run))
      printf("Could not launch File Viewer.\n");
  } else if (equals(line, "about"))
    printf("OSFOS kernel shell\nFramebuffer text UI with Virtual "
           "Framebuffer abstraction.\n");
  else if (equals(line, "smpinfo"))
    printf("Online processors: %u\n",
           static_cast<unsigned>(Kernel::SMP::online_cpu_count()));
  else if (equals(line, "debug"))
    show_debug_info();
  else if (equals(line, "mem"))
    printf("Memory APIs available in this build:\n  KernelHeap::malloc/free\n  "
           "PMM::allocate_page_block/free_page_block\n");
  else if (equals(line, "taskman")) {
    if (!launch_app("Task Manager", Apps::task_manager_run))
      printf("Could not launch Task Manager.\n");
  } else if (equals(line, "heap-test"))
    run_heap_test(args);
  else if (equals(line, "pmm-test"))
    run_pmm_test(args);
  else if (equals(line, "mema")) {
    if (!launch_app("Memoery Analyzer", Apps::memory_analyzer_run))
      printf("Could not launch graphics test.\n");
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
    printf("The kernel shell runs in ring 0; there is no userspace "
           "process to exit.\n");
  else if (equals(line, "spong")) {
    if (!launch_app("Spong", run_spong))
      printf("Could not launch Spong.\n");
  } else if (equals(line, "breakout")) {
    if (!launch_app("Breakout", run_breakout))
      printf("Could not launch Breakout.\n");
  } else if (equals(line, "doom1")) {
    if (!launch_app("Doom I", run_doom1))
      printf("Could not launch Doom I.\n");
  } else if (equals(line, "doom2")) {
    if (!launch_app("Doom II", run_doom2))
      printf("Could not launch Doom II.\n");
  } else if (equals(line, "tetris")) {
    if (!launch_app("Tetris", run_tetris))
      printf("Could not launch Tetris.\n");
  } else if (equals(line, "pacman")) {
    if (!launch_app("Pac-Man", run_pacman))
      printf("Could not launch Pac-Man.\n");
  } else if (equals(line, "gtest")) {
    if (!launch_app("Graphics test", run_graphic_test))
      printf("Could not launch graphics test.\n");
  } else if (equals(line, "stress")) {
    if (!launch_app("Stress test", run_stress_test))
      printf("Could not launch stress test.\n");
  } else if (equals(line, "bench")) {
    if (!launch_app("System benchmark", run_benchmark))
      printf("Could not launch system benchmark.\n");
  } else if (equals(line, "shitcell")) {
    if (!launch_app("Shitcell", run_shitty_excel))
      printf("Could not launch Shitcell.\n");
  } else if (equals(line, "pain")) {
    if (!launch_app("Pain", run_pain))
      printf("Could not launch Pain.\n");
  } else if (equals(line, "calc")) {
    if (!launch_app("Calculator", run_calc))
      printf("Could not launch calculator.\n");
  } else if (equals(line, "shitpad")) {
    if (!launch_app("Shitpad Editor", run_shitpad))
      printf("Could not launch calculator.\n");
  } else if (equals(line, "c3d")) {
    if (!launch_app("Cube3D", run_c3d))
      printf("Could not launch calculator.\n");
  } else
    printf("command not found: %s\nType 'help' to list commands.\n", line);
}

} // namespace

void run() {
  Kernel::Graphics::WindowManager::register_window(
      Kernel::Scheduler::current_thread_id(), "Shell", true);
  if (!Kernel::Programs::Terminal::initialize()) {
    Kernel::Graphics::WindowManager::unregister_window(
        Kernel::Scheduler::current_thread_id());
    return;
  }
  if (Kernel::Debug::g_kernel_debug_info.init_reclaimed) {
    printf("Reclaimed %zu bytes (%zu pages) of init memory.\n",
           Kernel::Debug::g_kernel_debug_info.reclaimed_init_bytes,
           Kernel::Debug::g_kernel_debug_info.reclaimed_init_pages);
  }
  size_t size = 0;
  printf("Embedded font size: %u bytes\n", size);
  printf("OSFOS kernel shell ready.\nType 'help' to get started.\n");
  char input[INPUT_SIZE];
  prompt();
  while (!Kernel::Scheduler::should_stop()) {
    if (!fgets(input, sizeof(input), stdin)) {
      if (Kernel::Scheduler::should_stop())
        break;
      continue;
    }
    size_t length = strlen(input);
    while (length && (input[length - 1] == '\n' || input[length - 1] == '\r'))
      input[--length] = '\0';
    execute(input);
    prompt();
  }
  Kernel::Programs::Terminal::shutdown();
}

} // namespace Kernel::Shell
