#include <kernel/bootinfo.h>
#include <kernel/cpu.h>
#include <kernel/drivers/e1000.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/drivers/pit.h>
#include <kernel/graphics/window_manager.h>
#include <kernel/lib/random.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/pmm.h>
#include <kernel/net/smoltcp_bridge.h>
#include <kernel/scheduler.h>
#include <kernel/shell/shell.h>

extern "C" void main(unsigned long multiboot_info_addr) {
  cpu_init();

  auto fb_info = Kernel::Boot::parse_framebuffer_info(multiboot_info_addr);

  Kernel::Drivers::Framebuffer::init({
      .address = fb_info.address,
      .width = fb_info.width,
      .height = fb_info.height,
      .pitch = fb_info.pitch,
      .format = fb_info.format,
  });

  const auto framebuffer_phys =
      reinterpret_cast<uintptr_t>(const_cast<void *>(fb_info.address));

  if (!Kernel::Boot::initialize_pmm_from_multiboot(
          multiboot_info_addr, framebuffer_phys, fb_info.pitch,
          fb_info.height)) {
    kernel_printf("PMM initialization failed: no usable RAM.\n");
    for (;;) {
      __asm__ volatile("cli; hlt");
    };
  }

  size_t initial_heap_pages = 16384;
  void *heap_physical_mem =
      Kernel::Memory::g_pmm.allocate_page_block(initial_heap_pages);

  if (heap_physical_mem != nullptr) {
    Kernel::Memory::g_kheap.init(
        heap_physical_mem, initial_heap_pages * Kernel::Memory::PMM::PAGE_SIZE);
  }

  const bool window_manager_ready =
      Kernel::Graphics::WindowManager::initialize();

  Kernel::Scheduler::scheduler_init();
  Kernel::Drivers::Keyboard::init();
  Kernel::Drivers::Mouse::init();
  Kernel::Drivers::PIT::init(250);

  if (Kernel::Drivers::E1000::detect_and_init()) {
    if (!Kernel::Drivers::E1000::start()) {
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
      kernel_printf("Network stack initialization failed.\n");
    }
  }

  if (window_manager_ready) {
    Kernel::Scheduler::thread_create(Kernel::Graphics::WindowManager::run);
  }
  Kernel::Scheduler::thread_create(Kernel::Shell::run);

  __asm__ volatile("sti");
  while (true) {
    __asm__ volatile("hlt");
  }
}