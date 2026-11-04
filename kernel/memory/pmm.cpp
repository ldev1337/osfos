#include <arch/x86_64/spinlock.h>
#include <arch/x86_64/string.h>
#include <kernel/memory/pmm.h>

#include <algorithm>

namespace Kernel::Memory {

alignas(64) PMM g_pmm;
static Spinlock pmm_lock;

// Two-bit page states:
//
// bitmap  state_bitmap  Meaning
//   0          0        Usable and free
//   1          0        Usable and allocated
//   0          1        Usable but permanently reserved
//   1          1        Not yet registered / unavailable

void PMM::set_bit(size_t page_index, bool value) {
  const size_t byte_index = page_index / 8;
  const uint8_t mask = static_cast<uint8_t>(1u << (page_index % 8));

  if (value)
    bitmap[byte_index] |= mask;
  else
    bitmap[byte_index] &= static_cast<uint8_t>(~mask);
}

bool PMM::get_bit(size_t page_index) const {
  const size_t byte_index = page_index / 8;
  const uint8_t mask = static_cast<uint8_t>(1u << (page_index % 8));

  return (bitmap[byte_index] & mask) != 0;
}

void PMM::set_state_bit(size_t page_index, bool value) {
  const size_t byte_index = page_index / 8;
  const uint8_t mask = static_cast<uint8_t>(1u << (page_index % 8));

  if (value)
    state_bitmap[byte_index] |= mask;
  else
    state_bitmap[byte_index] &= static_cast<uint8_t>(~mask);
}

bool PMM::get_state_bit(size_t page_index) const {
  const size_t byte_index = page_index / 8;
  const uint8_t mask = static_cast<uint8_t>(1u << (page_index % 8));

  return (state_bitmap[byte_index] & mask) != 0;
}

void PMM::init(void *bitmap_buffer, size_t mem_size) {
  const uint64_t flags = pmm_lock.lock_irqsave();

  bitmap = nullptr;
  state_bitmap = nullptr;

  bitmap_pages = 0;
  total_pages = 0;
  bitmap_size = 0;
  used_pages_count = 0;
  bootstrap_open = false;

  if (!bitmap_buffer || mem_size == 0) {
    pmm_lock.unlock_irqrestore(flags);
    return;
  }

  // mem_size represents the maximum physical address (exclusive),
  // not the sum of all available RAM regions.
  bitmap_pages = mem_size / PAGE_SIZE + (mem_size % PAGE_SIZE != 0);

  bitmap_size = bitmap_pages / 8 + (bitmap_pages % 8 != 0);

  const size_t SIZE_MAX_VALUE = static_cast<size_t>(-1);

  // Two bitmap planes must fit in the supplied buffer.
  if (bitmap_size > SIZE_MAX_VALUE / 2) {
    bitmap_pages = 0;
    bitmap_size = 0;

    pmm_lock.unlock_irqrestore(flags);
    return;
  }

  bitmap = static_cast<uint8_t *>(bitmap_buffer);
  state_bitmap = bitmap + bitmap_size;

  // Initially everything is unavailable.
  // The bootloader memory map will register actual usable regions.
  memset(bitmap, 0xFF, bitmap_size * 2);

  total_pages = 0;
  used_pages_count = 0;
  bootstrap_open = true;

  pmm_lock.unlock_irqrestore(flags);
}

void PMM::reserve_page(size_t page_index) {
  const uint64_t flags = pmm_lock.lock_irqsave();

  if (bitmap && page_index < bitmap_pages) {
    const bool used = get_bit(page_index);
    const bool state = get_state_bit(page_index);

    // State 11: not registered as usable RAM.
    // Leave it unavailable.
    if (!(used && state)) {

      // State 00: free usable page becoming reserved.
      if (!used && !state)
        ++used_pages_count;

      // Convert either free or allocated usable memory
      // into permanently reserved memory.
      set_bit(page_index, false);
      set_state_bit(page_index, true);
    }
  }

  pmm_lock.unlock_irqrestore(flags);
}

void *PMM::allocate_page_block(size_t count) {
  if (count == 0)
    return nullptr;

  const uint64_t flags = pmm_lock.lock_irqsave();

  if (!bitmap) {
    pmm_lock.unlock_irqrestore(flags);
    return nullptr;
  }

  // All usable regions must have been registered before
  // the first allocation.
  bootstrap_open = false;

  if (used_pages_count > total_pages ||
      count > total_pages - used_pages_count) {
    pmm_lock.unlock_irqrestore(flags);
    return nullptr;
  }

  size_t pages_found = 0;
  size_t start_page = 0;

  for (size_t i = 0; i < bitmap_pages; ++i) {
    // Only state 00 is allocatable.
    if (!get_bit(i) && !get_state_bit(i)) {
      if (pages_found == 0)
        start_page = i;

      ++pages_found;

      if (pages_found == count) {
        for (size_t j = start_page; j < start_page + count; ++j)
          set_bit(j, true);

        used_pages_count += count;

        const uintptr_t physical_address =
            static_cast<uintptr_t>(start_page * PAGE_SIZE);

        pmm_lock.unlock_irqrestore(flags);

        return reinterpret_cast<void *>(physical_address);
      }
    } else {
      pages_found = 0;
    }
  }

  pmm_lock.unlock_irqrestore(flags);
  return nullptr;
}

void PMM::free_page_block(void *ptr, size_t count) {
  if (count == 0)
    return;

  // Physical address zero is valid during memory-map registration.
  // Page zero must be reserved before the allocator starts.
  const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);

  if (address & (PAGE_SIZE - 1))
    return;

  const uint64_t flags = pmm_lock.lock_irqsave();

  const size_t start_page = address / PAGE_SIZE;

  if (!bitmap || start_page >= bitmap_pages) {
    pmm_lock.unlock_irqrestore(flags);
    return;
  }

  // Prevent integer overflow and out-of-range bitmap accesses.
  count = std::min(count, bitmap_pages - start_page);

  for (size_t j = start_page; j < start_page + count; ++j) {
    const bool used = get_bit(j);
    const bool state = get_state_bit(j);

    if (used && !state) {
      // State 10? No: state 01.
      // A normally allocated usable page.
      set_bit(j, false);

      if (used_pages_count != 0)
        --used_pages_count;
    } else if (used && state && bootstrap_open) {
      // State 11.
      // During boot, this operation registers a validated
      // usable memory-map region.
      //
      // Once the first allocation happens, bootstrap_open
      // becomes false and this operation is no longer allowed.

      set_bit(j, false);
      set_state_bit(j, false);

      if (total_pages < bitmap_pages)
        ++total_pages;
    }
  }

  pmm_lock.unlock_irqrestore(flags);
}

size_t PMM::recheck_used_memory() const {
  const uint64_t flags = pmm_lock.lock_irqsave();

  size_t used_pages = 0;

  for (size_t page = 0; page < bitmap_pages; ++page) {
    const bool used = get_bit(page);
    const bool state = get_state_bit(page);

    // States 01 and 10 are managed but unavailable.
    // States 00 and 11 are not counted as used.
    if (used != state)
      ++used_pages;
  }

  pmm_lock.unlock_irqrestore(flags);

  return used_pages * PAGE_SIZE;
}

void *PMM::allocate_huge_page() {
  constexpr size_t huge_page_pages = 512; // 2 MiB / 4 KiB per page
  const uint64_t flags = pmm_lock.lock_irqsave();

  if (!bitmap) {
    pmm_lock.unlock_irqrestore(flags);
    return nullptr;
  }

  bootstrap_open = false;

  size_t pages_found = 0;
  size_t start_page = 0;

  for (size_t i = 0; i < bitmap_pages; ++i) {
    // If we are looking for a start page, ensure it is 2MiB aligned (index
    // divisible by 512)
    if (pages_found == 0 && (i % huge_page_pages != 0)) {
      continue;
    }

    if (!get_bit(i) && !get_state_bit(i)) {
      if (pages_found == 0)
        start_page = i;

      ++pages_found;

      if (pages_found == huge_page_pages) {
        for (size_t j = start_page; j < start_page + huge_page_pages; ++j)
          set_bit(j, true);

        used_pages_count += huge_page_pages;

        const uintptr_t physical_address =
            static_cast<uintptr_t>(start_page * PAGE_SIZE);

        pmm_lock.unlock_irqrestore(flags);
        return reinterpret_cast<void *>(physical_address);
      }
    } else {
      pages_found = 0;
    }
  }

  pmm_lock.unlock_irqrestore(flags);
  return nullptr;
}

} // namespace Kernel::Memory