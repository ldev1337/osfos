#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Kernel::Memory{

class PMM {
private:
    uint8_t* bitmap;
    size_t total_pages;
    size_t bitmap_size;
    void set_bit(size_t page_index, bool value);
    bool get_bit(size_t page_index);

public:
    static const size_t PAGE_SIZE = 4096;
    void init(void* bitmap_buffer, size_t mem_size);
    void reserve_page(size_t page_index);
    void free_page_block(void* ptr, size_t count);
    void* allocate_page_block(size_t count);
};

extern PMM g_pmm;
}