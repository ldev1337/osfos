#ifndef KERNEL_BOOT_MULTIBOOT_H
#define KERNEL_BOOT_MULTIBOOT_H

#include <cstdint>
#include <kernel/drivers/framebuffer.h>

namespace Kernel::Boot {

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

struct multiboot_info {
  uint32_t total_size;
  uint32_t reserved;
};

struct multiboot_tag_mmap {
  uint32_t type;
  uint32_t size;
  uint32_t entry_size;
  uint32_t entry_version;
};

struct multiboot_mmap_entry {
  uint64_t addr;
  uint64_t len;
  uint32_t type;
  uint32_t reserved;
};

struct multiboot_tag_module {
  uint32_t type;
  uint32_t size;
  uint32_t mod_start;
  uint32_t mod_end;
};

struct FramebufferInfo {
  volatile void *address{nullptr};
  uint32_t width{0};
  uint32_t height{0};
  uint32_t pitch{0};
  Kernel::Drivers::Framebuffer::Format format{
      Kernel::Drivers::Framebuffer::Format::RGB32};
};

/**
 * Parses multiboot structures to retrieve framebuffer details.
 */
FramebufferInfo parse_framebuffer_info(unsigned long multiboot_info_addr);

/**
 * Initializes physical memory manager (PMM) by parsing Multiboot2 tags and
 * memory map.
 */
bool initialize_pmm_from_multiboot(unsigned long multiboot_info_addr,
                                   uintptr_t framebuffer_addr,
                                   uint32_t framebuffer_pitch,
                                   uint32_t framebuffer_height);

} // namespace Kernel::Boot

#endif // KERNEL_BOOT_MULTIBOOT_H