#include <arch/x86_64/spinlock.h>
#include <arch/x86_64/string.h>
#include <kernel/memory/pmm.h>

namespace Kernel::Memory {

PMM g_pmm;
static Spinlock pmm_lock;

void PMM::set_bit(size_t page_index, bool value) {
  size_t byte_index = page_index / 8;
  size_t bit_index = page_index % 8;
  if (value)
    bitmap[byte_index] |= (1 << bit_index);
  else
    bitmap[byte_index] &= ~(1 << bit_index);
}

bool PMM::get_bit(size_t page_index) {
  size_t byte_index = page_index / 8;
  size_t bit_index = page_index % 8;
  return (bitmap[byte_index] & (1 << bit_index)) != 0;
}

void PMM::init(void *bitmap_buffer, size_t mem_size) {
  const uint64_t flags = pmm_lock.lock_irqsave();
  total_pages = mem_size / PAGE_SIZE;
  bitmap_size = total_pages / 8;
  bitmap = (uint8_t *)bitmap_buffer;
  used_pages_count = 0;

  memset(bitmap, 0, bitmap_size);
  pmm_lock.unlock_irqrestore(flags);
}

void PMM::reserve_page(size_t page_index) {
  const uint64_t flags = pmm_lock.lock_irqsave();
  if (page_index < total_pages) {
    if (!get_bit(page_index)) {
      set_bit(page_index, true);
      used_pages_count++;
    }
  }
  pmm_lock.unlock_irqrestore(flags);
}

void *PMM::allocate_page_block(size_t count) {
  if (count == 0)
    return nullptr;
  const uint64_t flags = pmm_lock.lock_irqsave();
  size_t pages_found = 0;
  size_t start_page = 0;

  for (size_t i = 0; i < total_pages; i++) {
    if (!get_bit(i)) {
      if (pages_found == 0)
        start_page = i;
      pages_found++;
      if (pages_found == count) {
        for (size_t j = start_page; j < start_page + count; j++) {
          set_bit(j, true);
        }
        used_pages_count += count; // O(1) tracking
        pmm_lock.unlock_irqrestore(flags);
        return (void *)(start_page * PAGE_SIZE);
      }
    } else {
      pages_found = 0;
    }
  }
  pmm_lock.unlock_irqrestore(flags);
  return nullptr;
}

void PMM::free_page_block(void *ptr, size_t count) {
  if (!ptr || count == 0)
    return;
  const uint64_t flags = pmm_lock.lock_irqsave();
  size_t start_page = (size_t)ptr / PAGE_SIZE;
  size_t freed_actual = 0;

  for (size_t j = start_page; j < start_page + count; j++) {
    if (j < total_pages && get_bit(j)) {
      set_bit(j, false);
      freed_actual++;
    }
  }
  if (used_pages_count >= freed_actual) {
    used_pages_count -= freed_actual; // O(1) tracking
  } else {
    used_pages_count = 0;
  }
  pmm_lock.unlock_irqrestore(flags);
}

} // namespace Kernel::Memory