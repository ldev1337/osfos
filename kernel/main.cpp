#include <kernel/cpu.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/drivers/pit.h>
#include <kernel/graphics/window_manager.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/pmm.h>
#include <kernel/scheduler.h>
#include <kernel/shell/shell.h>

struct multiboot_tag {
  uint32_t type;
  uint32_t size;
};

struct multiboot_tag_framebuffer {
  uint32_t type;
  uint32_t size;
  uint64_t framebuffer_addr;
  uint32_t framebuffer_pitch;
  uint32_t framebuffer_width;
  uint32_t framebuffer_height;
  uint8_t framebuffer_bpp;
  uint8_t framebuffer_type;
  uint16_t reserved;
  uint8_t red_position;
  uint8_t red_mask_size;
  uint8_t green_position;
  uint8_t green_mask_size;
  uint8_t blue_position;
  uint8_t blue_mask_size;
  uint8_t reserved_position;
  uint8_t reserved_mask_size;
};

extern "C" void main(unsigned long multiboot_info_addr) {
  cpu_init();

  auto *tag = (struct multiboot_tag *)(multiboot_info_addr + 8);
  volatile void *fb_addr = nullptr;
  uint32_t fb_width = 0;
  uint32_t fb_height = 0;
  uint32_t fb_pitch = 0;
  Kernel::Drivers::Framebuffer::Format fb_format =
      Kernel::Drivers::Framebuffer::Format::RGB32;

  while (tag->type != 0) {
    if (tag->type == 8) {
      auto *fb_tag = (struct multiboot_tag_framebuffer *)tag;
      fb_addr = (volatile void *)(uintptr_t)fb_tag->framebuffer_addr;
      fb_width = fb_tag->framebuffer_width;
      fb_height = fb_tag->framebuffer_height;
      fb_pitch = fb_tag->framebuffer_pitch;
      const bool rgb_byte_order = fb_tag->framebuffer_type != 1 ||
                                  fb_tag->red_position < fb_tag->blue_position;
      const bool rgb565_order = fb_tag->framebuffer_type != 1 ||
                                fb_tag->red_position > fb_tag->blue_position;
      if (fb_tag->framebuffer_bpp == 32) {
        fb_format = rgb_byte_order
                        ? Kernel::Drivers::Framebuffer::Format::RGB32
                        : Kernel::Drivers::Framebuffer::Format::BGR32;
      } else if (fb_tag->framebuffer_bpp == 24) {
        fb_format = rgb_byte_order
                        ? Kernel::Drivers::Framebuffer::Format::RGB24
                        : Kernel::Drivers::Framebuffer::Format::BGR24;
      } else if (fb_tag->framebuffer_bpp == 16) {
        fb_format = rgb565_order ? Kernel::Drivers::Framebuffer::Format::RGB565
                                 : Kernel::Drivers::Framebuffer::Format::BGR565;
      }
      break;
    }
    tag = (struct multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7));
  }

  Kernel::Drivers::Framebuffer::init(
      {fb_addr, fb_width, fb_height, fb_pitch, fb_format});

  size_t total_ram = 4ULL * 1024 * 1024 * 1024;
  void *bitmap_safe_addr = (void *)0x200000;
  Kernel::Memory::g_pmm.init(bitmap_safe_addr, total_ram);
  for (size_t i = 0; i < 1024; i++) {
    Kernel::Memory::g_pmm.reserve_page(i);
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
  if (window_manager_ready)
    Kernel::Scheduler::thread_create(Kernel::Graphics::WindowManager::run);
  Kernel::Scheduler::thread_create(Kernel::Shell::run);

  __asm__ volatile("sti");
  while (true) {
    __asm__ volatile("hlt");
  }
}
