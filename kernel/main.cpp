#include <kernel/bootinfo.h>
#include <kernel/cpu.h>
#include <kernel/debug/debug.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/drivers/net/device.h>
#include <kernel/drivers/tsc_deadline.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/lwm.h>
#include <kernel/lib/lz4.h>
#include <kernel/lib/random.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/init.h>
#include <kernel/memory/pmm.h>
#include <kernel/net/smoltcp_bridge.h>
#include <kernel/programs/welcome.h>
#include <kernel/scheduler.h>
#include <kernel/shell/shell.h>
#include <kernel/smp.h>
#include <stddef.h>
#include <stdint.h>

namespace Kernel::Apps {
void benchmark_startup_run();
}

namespace Kernel::Debug {
struct KernelDebugInfo {
  size_t reclaimed_init_bytes;
  size_t reclaimed_init_pages;
  bool init_reclaimed;
};

extern KernelDebugInfo g_kernel_debug_info;
KernelDebugInfo g_kernel_debug_info{};
} // namespace Kernel::Debug

namespace {

uint64_t read_tsc_serialized() {
  uint32_t lo = 0, hi = 0;
  __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "rcx");
  return (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo);
}

void fill_repetitive(uint8_t *buf, size_t len) {
  static constexpr char kPattern[] =
      "osfos-lz4-test-payload-repeating-block-0123456789abcdef\n";
  constexpr size_t kPatLen = sizeof(kPattern) - 1;
  for (size_t i = 0; i < len; ++i) {
    buf[i] = static_cast<uint8_t>(kPattern[i % kPatLen]);
  }
}

void fill_pseudo_random(uint8_t *buf, size_t len, uint64_t seed) {
  uint64_t state = seed ? seed : 0x9e3779b97f4a7c15ULL;
  for (size_t i = 0; i < len; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    buf[i] = static_cast<uint8_t>(state & 0xFFu);
  }
}

bool buffers_equal(const uint8_t *a, const uint8_t *b, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

bool verify_roundtrip_case(const char *label, const uint8_t *src,
                           size_t src_len, uint8_t *comp_buf, size_t comp_cap,
                           uint8_t *decomp_buf, size_t decomp_cap,
                           uint32_t *hash_table) {
  int comp_ret = lib::lz4::compress(src, src_len, comp_buf, comp_cap,
                                    hash_table, lib::lz4::HASH_ENTRIES);
  if (comp_ret < 0) {
    Kernel::Debug::log(label ? label : "lz4");
    Kernel::Debug::log_dec("LZ4_FAIL_compress_err", comp_ret);
    return false;
  }

  size_t comp_size = static_cast<size_t>(comp_ret);
  int decomp_ret =
      lib::lz4::decompress(comp_buf, comp_size, decomp_buf, decomp_cap);
  if (decomp_ret < 0) {
    Kernel::Debug::log(label ? label : "lz4");
    Kernel::Debug::log_dec("LZ4_FAIL_decompress_err", decomp_ret);
    Kernel::Debug::log_dec("LZ4_FAIL_comp_size", comp_size);
    return false;
  }

  size_t decomp_size = static_cast<size_t>(decomp_ret);
  if (decomp_size != src_len) {
    Kernel::Debug::log(label ? label : "lz4");
    Kernel::Debug::log_dec("LZ4_FAIL_expected_len", src_len);
    Kernel::Debug::log_dec("LZ4_FAIL_got_len", decomp_size);
    return false;
  }

  if (src_len > 0 && !buffers_equal(src, decomp_buf, src_len)) {
    Kernel::Debug::log(label ? label : "lz4");
    Kernel::Debug::log("LZ4_FAIL_payload_mismatch");
    return false;
  }

  Kernel::Debug::log(label ? label : "lz4");
  Kernel::Debug::log_dec("LZ4_OK_raw_bytes", src_len);
  Kernel::Debug::log_dec("LZ4_OK_comp_bytes", comp_size);
  Kernel::Debug::log_dec("LZ4_OK_out_bytes", decomp_size);
  return true;
}

void run_lz4_test() {
  constexpr size_t kSize = 64 * 1024;
  constexpr size_t kIters = 16;

  Kernel::Debug::log("lz4 test start");

  auto *table = static_cast<uint32_t *>(
      malloc(sizeof(uint32_t) * lib::lz4::HASH_ENTRIES));
  size_t bound = lib::lz4::compress_bound(kSize);
  auto *src = static_cast<uint8_t *>(malloc(kSize));
  auto *comp = static_cast<uint8_t *>(malloc(bound));
  auto *out = static_cast<uint8_t *>(malloc(kSize));

  if (!table || !src || !comp || !out || bound == 0) {
    Kernel::Debug::log("lz4 test alloc failed");
    free(table);
    free(src);
    free(comp);
    free(out);
    return;
  }

  for (size_t i = 0; i < kSize; ++i) {
    src[i] = static_cast<uint8_t>((i * 37) % 13);
  }

  int c_len = lib::lz4::compress(src, kSize, comp, bound, table,
                                 lib::lz4::HASH_ENTRIES);
  int d_len =
      (c_len > 0)
          ? lib::lz4::decompress(comp, static_cast<size_t>(c_len), out, kSize)
          : -1;

  bool ok = (c_len > 0 && d_len == static_cast<int>(kSize));
  for (size_t i = 0; ok && i < kSize; ++i) {
    if (src[i] != out[i])
      ok = false;
  }
  Kernel::Debug::log(ok ? "lz4 roundtrip ok" : "lz4 roundtrip fail");
  Kernel::Debug::log_dec("lz4_comp_bytes",
                         c_len > 0 ? static_cast<size_t>(c_len) : 0);

  uint64_t t0 = Kernel::Lib::Random::rdtsc();
  for (size_t i = 0; i < kIters; ++i) {
    lib::lz4::compress(src, kSize, comp, bound, table, lib::lz4::HASH_ENTRIES);
  }
  uint64_t t1 = Kernel::Lib::Random::rdtsc();

  uint64_t t2 = Kernel::Lib::Random::rdtsc();
  for (size_t i = 0; i < kIters; ++i) {
    lib::lz4::decompress(comp, static_cast<size_t>(c_len > 0 ? c_len : 0), out,
                         kSize);
  }
  uint64_t t3 = Kernel::Lib::Random::rdtsc();

  uint64_t comp_cyc = t1 - t0;
  uint64_t decomp_cyc = t3 - t2;
  Kernel::Debug::log_dec("lz4_comp_cyc_per_byte", comp_cyc / (kSize * kIters));
  Kernel::Debug::log_dec("lz4_decomp_cyc_per_byte",
                         decomp_cyc / (kSize * kIters));

  free(table);
  free(src);
  free(comp);
  free(out);
  Kernel::Debug::log("lz4 test done");
}

} // namespace

extern "C" void main(unsigned long multiboot_info_addr) {
  Kernel::Debug::init();
  Kernel::Debug::log("boot entry");
  Kernel::Debug::log_hex("multiboot_info", multiboot_info_addr);

  Kernel::Debug::log("initializing CPU tables and checking required ISA");
  cpu_init();
  Kernel::Scheduler::scheduler_cpu_early_init();
  Kernel::Debug::log("cpu tables and feature check ready");

  Kernel::SMP::CpuInfo firmware_cpus[Kernel::SMP::MAX_CPUS]{};
  size_t firmware_cpu_count = 0;
  const bool have_cpu_topology =
      Kernel::SMP::discover(multiboot_info_addr, firmware_cpus,
                            Kernel::SMP::MAX_CPUS, &firmware_cpu_count);
  Kernel::Debug::log(have_cpu_topology ? "ACPI CPU topology found"
                                       : "ACPI CPU topology unavailable");
  Kernel::Debug::log_dec("firmware_cpu_count", firmware_cpu_count);

  auto fb_info = Kernel::Boot::parse_framebuffer_info(multiboot_info_addr);

  Kernel::Drivers::Framebuffer::init({
      .address = fb_info.address,
      .width = fb_info.width,
      .height = fb_info.height,
      .pitch = fb_info.pitch,
      .format = fb_info.format,
  });
  Kernel::Debug::log(Kernel::Drivers::Framebuffer::valid()
                         ? "framebuffer ready"
                         : "framebuffer unavailable");
  Kernel::Debug::log_dec("framebuffer_width", fb_info.width);
  Kernel::Debug::log_dec("framebuffer_height", fb_info.height);

  const auto framebuffer_phys =
      reinterpret_cast<uintptr_t>(const_cast<void *>(fb_info.address));

  if (!Kernel::Boot::initialize_pmm_from_multiboot(
          multiboot_info_addr, framebuffer_phys, fb_info.pitch,
          fb_info.height)) {
    Kernel::Debug::log("FATAL: PMM initialization failed");
    kprintf("PMM initialization failed: no usable RAM.\n");
    for (;;) {
      __asm__ volatile("cli; hlt");
    };
  }
  Kernel::Debug::log_dec("memory_total_bytes",
                         Kernel::Memory::g_pmm.get_total_memory());
  Kernel::Debug::log_dec("memory_free_bytes",
                         Kernel::Memory::g_pmm.get_free_memory());

  size_t initial_heap_pages = 64; // 256 KiB initial heap; expands dynamically
  void *heap_physical_mem =
      Kernel::Memory::g_pmm.allocate_page_block(initial_heap_pages);

  if (heap_physical_mem != nullptr) {
    Kernel::Memory::g_kheap.init(
        heap_physical_mem, initial_heap_pages * Kernel::Memory::PMM::PAGE_SIZE);
    Kernel::Debug::log_dec("heap_initial_bytes",
                           initial_heap_pages * Kernel::Memory::PMM::PAGE_SIZE);
  } else {
    Kernel::Debug::log("WARN: initial heap allocation failed");
  }

  const bool window_manager_ready =
      Kernel::Graphics::WindowManager::initialize();

  Kernel::Scheduler::scheduler_init();
  Kernel::Debug::log("scheduler ready");
  Kernel::Drivers::Keyboard::init();
  Kernel::Drivers::Mouse::init();
  if (!Kernel::Drivers::TSCDeadline::init(250)) {
    Kernel::Debug::log("FATAL: TSC-deadline timer initialization failed");
    for (;;)
      __asm__ volatile("cli; hlt");
  }
  Kernel::Debug::log("keyboard, mouse, TSC-deadline timer initialized");
  __asm__ volatile("sti");
  Kernel::FS::VFS::init();
  Kernel::Debug::log(Kernel::FS::VFS::is_mounted()
                         ? "VFS disk mounted"
                         : "VFS disk unavailable or unformatted");

  if (heap_physical_mem != nullptr) {
    run_lz4_test();
  }

  // ONLY UNCOMMENT FOR OPTIMIZING
  // Kernel::Debug::log("boot benchmark starting before welcome screen");
  // Kernel::Apps::benchmark_startup_run();

  if (Kernel::Drivers::Net::detect_and_init()) {
    const auto *net_device = Kernel::Drivers::Net::active_device();
    Kernel::Debug::log(net_device ? net_device->name : "network device ready");
    if (!Kernel::Drivers::Net::start()) {
      Kernel::Debug::log("ERROR: network device start failed");
      kprintf("Network device initialization failed.\n");
    }
    uint8_t mac[6];
    Kernel::Drivers::Net::get_mac_address(mac);
    uint64_t seed = Kernel::Lib::Random::rdtsc() ^
                    Kernel::Drivers::TSCDeadline::get_ticks();
    for (unsigned char iii : mac) {
      seed = (seed << 8) ^ iii;
    }
    if (Kernel::Net::initialize(mac, seed) != 0) {
      Kernel::Debug::log("ERROR: network stack initialization failed");
      kprintf("Network stack initialization failed.\n");
    } else {
      Kernel::Debug::log("network stack ready");
    }
  } else {
    Kernel::Debug::log("no supported network device detected");
  }

  if (have_cpu_topology) {
    const size_t ap_count = Kernel::SMP::launch_application_processors(
        firmware_cpus, firmware_cpu_count);
    kprintf("SMP: %u/%u application processor(s) online.\n",
            static_cast<unsigned>(ap_count),
            static_cast<unsigned>(firmware_cpu_count - 1));
    Kernel::Debug::log_dec("application_processors_online", ap_count);
  } else {
    kprintf("SMP: no usable ACPI processor topology; continuing on BSP.\n");
  }
  Kernel::Debug::log("boot initialization complete; showing welcome screen");
  Kernel::Programs::show();

  if (window_manager_ready) {
    const int compositor_id = Kernel::Scheduler::thread_create(
        Kernel::Graphics::WindowManager::run,
        Kernel::Scheduler::ThreadPriority::HIGH);
    if (compositor_id > 0)
      (void)Kernel::Scheduler::prefer_bootstrap_cpu(
          static_cast<uint64_t>(compositor_id));
    else
      Kernel::Debug::log("ERROR: compositor thread creation failed");
  }
  if (Kernel::Scheduler::thread_create(Kernel::Shell::run) <= 0)
    Kernel::Debug::log("ERROR: shell thread creation failed");

  {
    uintptr_t init_start_phys = reinterpret_cast<uintptr_t>(_os_init_start);
    uintptr_t init_end_phys = reinterpret_cast<uintptr_t>(_os_init_end);

    if (init_end_phys > init_start_phys) {
      size_t total_bytes = init_end_phys - init_start_phys;
      size_t page_count = (total_bytes + Kernel::Memory::PMM::PAGE_SIZE - 1) /
                          Kernel::Memory::PMM::PAGE_SIZE;

      Kernel::Memory::g_pmm.free_page_block(
          reinterpret_cast<void *>(init_start_phys), page_count);

      // Store in the debug info struct
      Kernel::Debug::g_kernel_debug_info = {
          .reclaimed_init_bytes = total_bytes,
          .reclaimed_init_pages = page_count,
          .init_reclaimed = true,
      };
    }
  }
  while (true) {
    __asm__ volatile("hlt");
  }
}
