#pragma once
#include <cstddef>
#include <cstdint>

namespace Kernel::Memory {

class PMM {
private:
  uint8_t *bitmap;
  uint8_t *state_bitmap;

  size_t bitmap_pages; // Highest physical page index + 1
  size_t total_pages;  // Actual usable pages registered from mmap
  size_t bitmap_size;
  size_t used_pages_count;
  size_t hint_word;

  bool bootstrap_open;

  void set_bit(size_t page_index, bool value);
  bool get_bit(size_t page_index) const;

  void set_state_bit(size_t page_index, bool value);
  bool get_state_bit(size_t page_index) const;

public:
  static const size_t PAGE_SIZE = 4096;

  void init(void *bitmap_buffer, size_t mem_size);
  void reserve_page(size_t page_index);
  void free_page_block(void *ptr, size_t count);
  void *allocate_page_block(size_t count);

  // RAM analytics
  size_t get_total_memory() const { return total_pages * PAGE_SIZE; }

  size_t get_used_memory() const { return used_pages_count * PAGE_SIZE; }

  size_t get_free_memory() const {
    return (total_pages - used_pages_count) * PAGE_SIZE;
  }

  size_t recheck_used_memory() const;

  size_t get_total_pages() const { return total_pages; }

  size_t get_used_pages() const { return used_pages_count; }

  size_t get_free_pages() const { return total_pages - used_pages_count; }

  void *allocate_huge_page();
};

extern PMM g_pmm;

} // namespace Kernel::Memory