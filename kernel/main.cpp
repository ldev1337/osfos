#include <kernel/bootinfo.h>
#include <kernel/cpu.h>
#include <kernel/drivers/e1000.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/drivers/pit.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/lwm.h>
#include <kernel/lib/random.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/init.h>
#include <kernel/memory/pmm.h>
#include <kernel/net/smoltcp_bridge.h>
#include <kernel/scheduler.h>
#include <kernel/shell/shell.h>
#include <kernel/smp.h>
#include <kernel/debug/debug.h>
#include <kernel/programs/welcome.h>

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

extern "C" void main(unsigned long multiboot_info_addr) {
  Kernel::Debug::init();
  Kernel::Debug::log("boot entry");
  Kernel::Debug::log_hex("multiboot_info", multiboot_info_addr);

  Kernel::Debug::log("initializing CPU tables and checking required ISA");
  cpu_init();
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
    kernel_printf("PMM initialization failed: no usable RAM.\n");
    for (;;) {
      __asm__ volatile("cli; hlt");
    };
  }
  Kernel::Debug::log_dec("memory_total_bytes",
                         Kernel::Memory::g_pmm.get_total_memory());
  Kernel::Debug::log_dec("memory_free_bytes",
                         Kernel::Memory::g_pmm.get_free_memory());

  size_t initial_heap_pages = 16384;
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
  Kernel::Drivers::PIT::init(250);
  Kernel::Debug::log("keyboard, mouse, PIT initialized");
  __asm__ volatile("sti");
  Kernel::FS::VFS::init();
  Kernel::Debug::log(Kernel::FS::VFS::is_mounted()
                         ? "VFS disk mounted"
                         : "VFS disk unavailable or unformatted");
  Kernel::Debug::log("boot benchmark starting before welcome screen");
  Kernel::Apps::benchmark_startup_run();

  if (Kernel::Drivers::E1000::detect_and_init()) {
    Kernel::Debug::log("E1000 detected and initialized");
    if (!Kernel::Drivers::E1000::start()) {
      Kernel::Debug::log("ERROR: E1000 RX worker failed");
      kernel_printf("E1000 RX worker initialization failed.\n");
    }
    uint8_t mac[6];
    Kernel::Drivers::E1000::get_mac_address(mac);
    uint64_t seed =
        Kernel::Lib::Random::rdtsc() ^ Kernel::Drivers::PIT::get_ticks();
    for (unsigned char iii : mac) {
      seed = (seed << 8) ^ iii;
    }
    if (Kernel::Net::initialize(mac, seed) != 0) {
      Kernel::Debug::log("ERROR: network stack initialization failed");
      kernel_printf("Network stack initialization failed.\n");
    } else {
      Kernel::Debug::log("network stack ready");
    }
  } else {
    Kernel::Debug::log("E1000 not detected; network disabled");
  }

  if (have_cpu_topology) {
    const size_t ap_count = Kernel::SMP::launch_application_processors(
        firmware_cpus, firmware_cpu_count);
    kernel_printf("SMP: %u/%u application processor(s) online.\n",
                  static_cast<unsigned>(ap_count),
                  static_cast<unsigned>(firmware_cpu_count - 1));
    Kernel::Debug::log_dec("application_processors_online", ap_count);
  } else {
    kernel_printf(
        "SMP: no usable ACPI processor topology; continuing on BSP.\n");
  }
  Kernel::Debug::log("boot initialization complete; showing welcome screen");
  Kernel::Programs::show();
  Kernel::Programs::wait_seconds(1);

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
