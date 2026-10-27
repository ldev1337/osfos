#include <kernel/memory/pmm.h>
#include <arch/x86_64/string.h>

namespace Kernel::Memory{
PMM g_pmm;

void PMM::set_bit(size_t page_index, bool value) {
    size_t byte_index = page_index / 8;
    size_t bit_index  = page_index % 8;
    if (value) bitmap[byte_index] |= (1 << bit_index);
    else       bitmap[byte_index] &= ~(1 << bit_index);
}

bool PMM::get_bit(size_t page_index) {
    size_t byte_index = page_index / 8;
    size_t bit_index  = page_index % 8;
    return (bitmap[byte_index] & (1 << bit_index)) != 0;
}

void PMM::init(void* bitmap_buffer, size_t mem_size) {
    total_pages = mem_size / PAGE_SIZE;
    bitmap_size = total_pages / 8;
    bitmap = (uint8_t*)bitmap_buffer;

    memset(bitmap, 0, bitmap_size);
}

void PMM::reserve_page(size_t page_index) {
    if (page_index < total_pages) set_bit(page_index, true);
}

void* PMM::allocate_page_block(size_t count) {
    if (count == 0) return nullptr;
    size_t pages_found = 0;
    size_t start_page = 0;

    for (size_t i = 0; i < total_pages; i++) {
        if (!get_bit(i)) {
            if (pages_found == 0) start_page = i;
            pages_found++;
            if (pages_found == count) {
                for (size_t j = start_page; j < start_page + count; j++) set_bit(j, true);
                return (void*)(start_page * PAGE_SIZE);
            }
        } else {
            pages_found = 0;
        }
    }
    return nullptr;
}

void PMM::free_page_block(void* ptr, size_t count) {
    if (!ptr || count == 0) return;
    size_t start_page = (size_t)ptr / PAGE_SIZE;
    for (size_t j = start_page; j < start_page + count; j++) {
        if (j < total_pages) set_bit(j, false);
    }
}
}