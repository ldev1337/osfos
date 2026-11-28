#include <kernel/debug/debug.h>
#include <kernel/drivers/net/device.h>
#include <kernel/drivers/pci.h>
#include <kernel/drivers/usb/xhci.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/open_font.h>
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
int cli_args_test(int argc, char **argv, void *context);
int cli_exitcode_test(int argc, char **argv, void *context);
int cli_stdio_test(int argc, char **argv, void *context);
int cli_memory_lifetime_test(int argc, char **argv, void *context);
} // namespace Kernel::Apps

namespace Kernel::Shell {

// --- Constants & Theme ---
constexpr size_t INPUT_SIZE = 256;

namespace {
struct ProgramCommand {
  const char *name;
  const char *title;
  void (*app_entry)();
  int (*cli_entry)(int, char **, void *);
  void *context;
  bool graphical;
};

int execute_program_process(int argc, char **argv, void *context) {
  auto *program = static_cast<const ProgramCommand *>(context);
  if (!program)
    return 127;
  Kernel::Scheduler::mark_current_thread_as_application(program->title);
  if (program->graphical &&
      Kernel::Graphics::WindowManager::register_window(
          Kernel::Scheduler::current_thread_id(), program->title) < 0) {
    printf("%s: could not create application window\n", program->name);
    return 1;
  }
  if (program->cli_entry)
    return program->cli_entry(argc, argv, program->context);
  if (program->app_entry)
    program->app_entry();
  if (program->graphical)
    Kernel::Scheduler::process_exit(0);
  return 0;
}

bool launch_program(const ProgramCommand &program, int argc, char **argv) {
  const uint64_t pid = Kernel::Scheduler::process_spawn(
      execute_program_process, argc, const_cast<const char *const *>(argv),
      const_cast<ProgramCommand *>(&program), program.title);
  if (!pid) {
    printf("%s: process creation failed\n", program.name);
    return false;
  }
  int status = 0;
  if (Kernel::Scheduler::process_wait(pid, &status) != 0) {
    printf("%s: wait failed for process %llu\n", program.name,
           static_cast<unsigned long long>(pid));
    return false;
  }
  if (status != 0)
    printf("%s: exited with status %d\n", program.name, status);
  return status == 0;
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
  printf("OSFOS shell commands:\n");
  printf("  help, clear, echo, exit, about, debug, smpinfo, taskman\n");
  printf("  ping, netup, netinfo, tcp-echo, fetch, youtube, fontcheck\n");
  printf("  heap-test, pmm-test, disk-init, disk-check, xhci-init, ls, cat\n");
  printf("  spong, breakout, doom1, doom2, tetris, pacman, gtest, stress\n");
  printf("  bench, shitcell, shitpad, pain, calc, c3d, fontview, fileview\n");
  printf("  args [values...], exitcode [status], stdio-test, memlife\n");
  printf("Programs run as child processes; the shell waits for each one.\n");
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

size_t parse_arguments(char *line, char **argv, size_t capacity) {
  size_t argc = 0;
  char *read = line;
  char *write = line;
  while (*read) {
    while (*read == ' ' || *read == '\t')
      ++read;
    if (!*read)
      break;
    if (argc == capacity)
      return 0;
    argv[argc++] = write;
    char quote = 0;
    bool started = false;
    while (*read) {
      const char ch = *read++;
      if (!quote && (ch == ' ' || ch == '\t'))
        break;
      if (ch == '\\' && quote != '\'') {
        if (*read) {
          *write++ = *read++;
          started = true;
        }
        continue;
      }
      if (ch == '\'' || ch == '"') {
        if (!quote) {
          quote = ch;
          started = true;
          continue;
        }
        if (quote == ch) {
          quote = 0;
          started = true;
          continue;
        }
      }
      *write++ = ch;
      started = true;
    }
    if (quote)
      return 0;
    (void)started;
    *write++ = '\0';
  }
  return argc;
}

void join_arguments(char **argv, size_t first, size_t argc, char *out,
                    size_t capacity) {
  size_t used = 0;
  if (!capacity)
    return;
  out[0] = '\0';
  for (size_t i = first; i < argc; ++i) {
    if (i != first && used + 1 < capacity)
      out[used++] = ' ';
    for (const char *p = argv[i]; *p && used + 1 < capacity; ++p)
      out[used++] = *p;
  }
  out[used] = '\0';
}

const ProgramCommand program_commands[] = {
    {"spong", "Spong", run_spong, nullptr, nullptr, true},
    {"breakout", "Breakout", run_breakout, nullptr, nullptr, true},
    {"doom1", "Doom I", run_doom1, nullptr, nullptr, true},
    {"doom2", "Doom II", run_doom2, nullptr, nullptr, true},
    {"tetris", "Tetris", run_tetris, nullptr, nullptr, true},
    {"pacman", "Pac-Man", run_pacman, nullptr, nullptr, true},
    {"gtest", "Graphics Test", run_graphic_test, nullptr, nullptr, true},
    {"stress", "Stress Test", run_stress_test, nullptr, nullptr, true},
    {"bench", "Benchmark", run_benchmark, nullptr, nullptr, true},
    {"shitcell", "Shitcell", run_shitty_excel, nullptr, nullptr, true},
    {"shitpad", "Shitpad", run_shitpad, nullptr, nullptr, true},
    {"pain", "Pain", run_pain, nullptr, nullptr, true},
    {"calc", "Calculator", run_calc, nullptr, nullptr, true},
    {"c3d", "Cube3D", run_c3d, nullptr, nullptr, true},
    {"youtube", "YouTube", run_youtube, nullptr, nullptr, true},
    {"fontview", "Font Viewer", Apps::font_viewer_run, nullptr, nullptr, true},
    {"fileview", "File Viewer", Apps::file_viewer_run, nullptr, nullptr, true},
    {"taskman", "Task Manager", Apps::task_manager_run, nullptr, nullptr, true},
    {"mema", "Memory Analyzer", Apps::memory_analyzer_run, nullptr, nullptr,
     true},
    {"args", "Argument Test", nullptr, Apps::cli_args_test, nullptr, false},
    {"exitcode", "Exit Status Test", nullptr, Apps::cli_exitcode_test, nullptr,
     false},
    {"stdio-test", "stdio Test", nullptr, Apps::cli_stdio_test, nullptr, false},
    {"memlife", "Memory Lifetime Test", nullptr, Apps::cli_memory_lifetime_test,
     nullptr, false},
};

const ProgramCommand *find_program(const char *name) {
  for (const auto &program : program_commands)
    if (equals(name, program.name))
      return &program;
  return nullptr;
}

void execute(char *line) {
  char *argv[32]{};
  const size_t argc =
      parse_arguments(line, argv, sizeof(argv) / sizeof(argv[0]));
  if (!argc) {
    for (const char *p = line; *p; ++p)
      if (*p != ' ' && *p != '\t') {
        printf("syntax error or too many arguments\n");
        break;
      }
    return;
  }
  char args[INPUT_SIZE]{};
  join_arguments(argv, 1, argc, args, sizeof(args));
  const char *command = argv[0];

  if (equals(command, "help"))
    help();
  else if (equals(command, "clear"))
    printf("\f");
  else if (equals(command, "echo"))
    printf("%s\n", args);
  else if (equals(command, "ping"))
    run_ping(argc > 1 ? argv[1] : const_cast<char *>(""));
  else if (equals(command, "netup"))
    run_netup(args);
  else if (equals(command, "netinfo"))
    run_netinfo(args);
  else if (equals(command, "tcp-echo"))
    run_tcp_echo(args);
  else if (equals(command, "fetch"))
    run_fetch(args);
  else if (equals(command, "fontcheck"))
    run_fontcheck(args);
  else if (equals(command, "about"))
    printf(
        "OSFOS kernel shell: ring-0 command interpreter with process wait.\n");
  else if (equals(command, "smpinfo"))
    printf("Online processors: %u\n",
           static_cast<unsigned>(Kernel::SMP::online_cpu_count()));
  else if (equals(command, "debug"))
    show_debug_info();
  else if (equals(command, "mem"))
    printf("Memory APIs: KernelHeap::malloc/free and PMM page allocation.\n");
  else if (equals(command, "heap-test"))
    run_heap_test(args);
  else if (equals(command, "pmm-test"))
    run_pmm_test(args);
  else if (equals(command, "disk-init"))
    run_disk_init(args);
  else if (equals(command, "xhci-init"))
    run_xhci_init(args);
  else if (equals(command, "disk-check"))
    run_disk_check(args);
  else if (equals(command, "ls"))
    run_ls(args);
  else if (equals(command, "cat"))
    run_cat(args);
  else if (equals(command, "exit")) {
    Kernel::Scheduler::process_exit(0);
  } else if (const ProgramCommand *program = find_program(command)) {
    (void)launch_program(*program, static_cast<int>(argc), argv);
  } else {
    printf("command not found: %s\nType 'help' to list commands.\n", command);
  }
}

} // namespace

int process_entry(int, char **, void *) {
  run();
  return 0;
}

void run() {
  Kernel::Scheduler::mark_current_thread_as_application("Shell");
  Kernel::Graphics::WindowManager::register_window(
      Kernel::Scheduler::current_thread_id(), "Shell", true);
  if (!Kernel::Programs::Terminal::initialize())
    return;
  Kernel::Scheduler::process_set_terminal_thread_id(
      Kernel::Scheduler::current_thread_id());
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
}

} // namespace Kernel::Shell
