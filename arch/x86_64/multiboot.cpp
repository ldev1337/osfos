#include <algorithm>
#include <kernel/bootinfo.h>
#include <kernel/memory/pmm.h>

extern "C" uint8_t _kernel_end[];

namespace Kernel::Boot {

static const multiboot_tag_mmap *
find_mmap_tag(uintptr_t info_addr, const multiboot_info **info_out) {
  if (!info_addr)
    return nullptr;

  const auto *base = reinterpret_cast<const uint8_t *>(info_addr);
  const auto *info = reinterpret_cast<const multiboot_info *>(base);

  if (info->total_size < 16 ||
      info_addr > static_cast<uintptr_t>(-1) - info->total_size)
    return nullptr;

  const uint8_t *end = base + info->total_size;
  const uint8_t *cursor = base + 8;

  while (static_cast<size_t>(end - cursor) >= sizeof(multiboot_tag)) {
    const auto *tag = reinterpret_cast<const multiboot_tag *>(cursor);
    const size_t remaining = end - cursor;

    if (tag->size < 8 || tag->size > remaining)
      return nullptr;

    if (tag->type == 0)
      break;

    if (tag->type == 6) {
      if (tag->size < sizeof(multiboot_tag_mmap))
        return nullptr;

      const auto *mmap = reinterpret_cast<const multiboot_tag_mmap *>(tag);

      if (mmap->entry_size < sizeof(multiboot_mmap_entry))
        return nullptr;

      *info_out = info;
      return mmap;
    }

    const size_t aligned_size =
        (static_cast<size_t>(tag->size) + 7u) & ~size_t(7u);

    if (aligned_size > remaining)
      return nullptr;

    cursor += aligned_size;
  }

  return nullptr;
}

static void reserve_physical_range(uint64_t start, uint64_t end,
                                   size_t max_pages) {
  if (end <= start)
    return;

  constexpr uint64_t PAGE = Kernel::Memory::PMM::PAGE_SIZE;

  const uint64_t first_page = start / PAGE;

  uint64_t last_page = end / PAGE;
  if (end % PAGE)
    ++last_page;

  if (first_page >= max_pages)
    return;

  last_page = std::min(last_page, max_pages);

  for (uint64_t page = first_page; page < last_page; ++page)
    Kernel::Memory::g_pmm.reserve_page(static_cast<size_t>(page));
}

FramebufferInfo parse_framebuffer_info(unsigned long multiboot_info_addr) {
  FramebufferInfo fb{};
  auto *tag = reinterpret_cast<struct multiboot_tag *>(multiboot_info_addr + 8);

  while (tag->type != 0) {
    if (tag->type == 8) {
      auto *fb_tag = reinterpret_cast<struct multiboot_tag_framebuffer *>(tag);
      fb.address = reinterpret_cast<volatile void *>(
          static_cast<uintptr_t>(fb_tag->framebuffer_addr));
      fb.width = fb_tag->framebuffer_width;
      fb.height = fb_tag->framebuffer_height;
      fb.pitch = fb_tag->framebuffer_pitch;

      const bool rgb_byte_order = fb_tag->framebuffer_type != 1 ||
                                  fb_tag->red_position < fb_tag->blue_position;
      const bool rgb565_order = fb_tag->framebuffer_type != 1 ||
                                fb_tag->red_position > fb_tag->blue_position;

      if (fb_tag->framebuffer_bpp == 32) {
        fb.format = rgb_byte_order
                        ? Kernel::Drivers::Framebuffer::Format::RGB32
                        : Kernel::Drivers::Framebuffer::Format::BGR32;
      } else if (fb_tag->framebuffer_bpp == 24) {
        fb.format = rgb_byte_order
                        ? Kernel::Drivers::Framebuffer::Format::RGB24
                        : Kernel::Drivers::Framebuffer::Format::BGR24;
      } else if (fb_tag->framebuffer_bpp == 16) {
        fb.format = rgb565_order ? Kernel::Drivers::Framebuffer::Format::RGB565
                                 : Kernel::Drivers::Framebuffer::Format::BGR565;
      }
      break;
    }
    tag = reinterpret_cast<struct multiboot_tag *>(
        reinterpret_cast<uint8_t *>(tag) + ((tag->size + 7) & ~7));
  }

  return fb;
}

bool initialize_pmm_from_multiboot(unsigned long multiboot_info_addr,
                                   uintptr_t framebuffer_addr,
                                   uint32_t framebuffer_pitch,
                                   uint32_t framebuffer_height) {
  constexpr uint64_t PAGE = Kernel::Memory::PMM::PAGE_SIZE;
  constexpr uint64_t U64_MAX = ~uint64_t{0};

  const multiboot_info *info = nullptr;
  const auto *mmap =
      find_mmap_tag(static_cast<uintptr_t>(multiboot_info_addr), &info);

  if (!mmap || !info || mmap->size < 16 ||
      mmap->entry_size < sizeof(multiboot_mmap_entry))
    return false;

  const uint8_t *map_begin = reinterpret_cast<const uint8_t *>(mmap);
  const size_t map_size = mmap->size;
  const size_t entry_size = mmap->entry_size;

  // Pass 1: discover the highest usable physical address.
  uint64_t max_usable_end = 0;

  for (size_t offset = 16;
       offset <= map_size && map_size - offset >= entry_size;
       offset += entry_size) {

    const auto *region =
        reinterpret_cast<const multiboot_mmap_entry *>(map_begin + offset);

    if (region->type != 1 || region->len == 0 ||
        region->len > U64_MAX - region->addr)
      continue;

    const uint64_t region_end = region->addr + region->len;

    if (region_end > max_usable_end)
      max_usable_end = region_end;
  }

  if (max_usable_end == 0 ||
      max_usable_end > static_cast<uint64_t>(static_cast<size_t>(-1)))
    return false;

  const size_t max_pages =
      static_cast<size_t>(max_usable_end / PAGE + (max_usable_end % PAGE != 0));

  const size_t bitmap_bytes = max_pages / 8 + (max_pages % 8 != 0);

  if (bitmap_bytes > static_cast<size_t>(-1) / 2)
    return false;

  const size_t bitmap_storage = bitmap_bytes * 2;
  const uintptr_t bitmap_start = reinterpret_cast<uintptr_t>(_kernel_end);

  if (bitmap_storage >
      static_cast<size_t>(static_cast<uintptr_t>(-1) - bitmap_start))
    return false;

  const uintptr_t bitmap_end = bitmap_start + bitmap_storage;

  if (bitmap_end > max_usable_end)
    return false;

  Kernel::Memory::g_pmm.init(reinterpret_cast<void *>(bitmap_start),
                             static_cast<size_t>(max_usable_end));

  // Pass 2: register complete usable pages.
  for (size_t offset = 16;
       offset <= map_size && map_size - offset >= entry_size;
       offset += entry_size) {

    const auto *region =
        reinterpret_cast<const multiboot_mmap_entry *>(map_begin + offset);

    if (region->type != 1 || region->len == 0 ||
        region->len > U64_MAX - region->addr)
      continue;

    const uint64_t region_end = region->addr + region->len;

    uint64_t aligned_start = region->addr;
    const uint64_t remainder = aligned_start % PAGE;

    if (remainder != 0) {
      const uint64_t adjustment = PAGE - remainder;

      if (aligned_start > U64_MAX - adjustment)
        continue;

      aligned_start += adjustment;
    }

    const uint64_t aligned_end = region_end & ~(PAGE - 1);

    if (aligned_end <= aligned_start)
      continue;

    const uint64_t page_count = (aligned_end - aligned_start) / PAGE;

    if (page_count > static_cast<uint64_t>(static_cast<size_t>(-1)))
      return false;

    Kernel::Memory::g_pmm.free_page_block(
        reinterpret_cast<void *>(static_cast<uintptr_t>(aligned_start)),
        static_cast<size_t>(page_count));
  }

  // Reserve ranges
  reserve_physical_range(0, bitmap_end, max_pages);

  const uint64_t info_start = multiboot_info_addr;
  if (info->total_size > U64_MAX - info_start)
    return false;

  reserve_physical_range(info_start, info_start + info->total_size, max_pages);

  const auto *base = reinterpret_cast<const uint8_t *>(multiboot_info_addr);
  const uint8_t *info_end = base + info->total_size;
  const uint8_t *cursor = base + 8;

  while (cursor <= info_end && static_cast<size_t>(info_end - cursor) >= 8) {
    const auto *tag = reinterpret_cast<const multiboot_tag *>(cursor);
    const size_t remaining = info_end - cursor;

    if (tag->size < 8 || tag->size > remaining || tag->type == 0)
      break;

    if (tag->type == 3 && tag->size >= sizeof(multiboot_tag_module)) {
      const auto *module = reinterpret_cast<const multiboot_tag_module *>(tag);
      if (module->mod_end > module->mod_start) {
        reserve_physical_range(module->mod_start, module->mod_end, max_pages);
      }
    }

    const size_t aligned_size =
        (static_cast<size_t>(tag->size) + 7u) & ~size_t(7u);

    if (aligned_size > remaining)
      break;

    cursor += aligned_size;
  }

  const uint64_t framebuffer_size =
      static_cast<uint64_t>(framebuffer_pitch) * framebuffer_height;

  if (framebuffer_addr != 0 && framebuffer_size != 0 &&
      framebuffer_size <= U64_MAX - framebuffer_addr) {
    reserve_physical_range(framebuffer_addr,
                           framebuffer_addr + framebuffer_size, max_pages);
  }

  return Kernel::Memory::g_pmm.get_total_pages() != 0;
}

} // namespace Kernel::Boot