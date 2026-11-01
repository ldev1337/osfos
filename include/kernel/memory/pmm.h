#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Kernel::Memory{

class PMM {
private:
    uint8_t* bitmap;
    size_t total_pages;
    size_t bitmap_size;
    size_t used_pages_count; // O(1) counter

    void set_bit(size_t page_index, bool value);
    bool get_bit(size_t page_index);

public:
    static const size_t PAGE_SIZE = 4096;
    void init(void* bitmap_buffer, size_t mem_size);
    void reserve_page(size_t page_index);
    void free_page_block(void* ptr, size_t count);
    void* allocate_page_block(size_t count);

    // Fast O(1) RAM Analytics Getters
    size_t get_total_memory() const { return total_pages * PAGE_SIZE; }
    size_t get_used_memory() const { return used_pages_count * PAGE_SIZE; }
    size_t get_free_memory() const { return (total_pages - used_pages_count) * PAGE_SIZE; }
    size_t get_total_pages() const { return total_pages; }
    size_t get_used_pages() const { return used_pages_count; }
    size_t get_free_pages() const { return total_pages - used_pages_count; }
};

extern PMM g_pmm;
}