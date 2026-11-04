#pragma once
#include <cstddef>
#include <cstdint>
namespace Kernel::Memory {
struct HeapNode {
  size_t size;
  bool is_free;
  uint64_t owner_thread_id;
  HeapNode *next;
};
struct SystemMemoryInfo {
  size_t pmm_total_memory;
  size_t pmm_used_memory;
  size_t pmm_free_memory;
  size_t heap_total_size;
  size_t heap_used_size;
  size_t heap_free_size;
  size_t heap_peak_size;
};
class KernelHeap {
private:
  void *heap_begin;
  void *heap_end;
  void *first_block;
  void *free_bins[64][8];
  uint8_t sl_bitmap[64];
  uint64_t fl_bitmap;
  void *slab_free[12];
  void *slab_pages;
  size_t total_heap_size;
  size_t used_bytes_count;
  size_t peak_used_size;
  void insert_free_block(void *block);
  void remove_free_block(void *block);
  void *find_suitable_block(size_t size);
  void *allocate_block(size_t payload_size, size_t alignment, uint32_t flags,
                       void **block_out);
  void *release_block(void *block);
  void add_slab_free_page(void *page);
  void remove_slab_free_page(void *page);
  void unlink_slab_page(void *page);
  void release_slab_page(void *page);
  void *allocate_locked(size_t size, uint64_t owner);
  void *allocate_slab_locked(unsigned class_index, size_t size, uint64_t owner);
  void free_slab_locked(void *ptr, void *header, void *page);
  void free_locked(void *ptr);
  bool resize_large_locked(void *raw_block, size_t size);

public:
  void init(void *heap_start_addr, size_t initial_size);
  void *kmalloc(size_t size);
  void *kmalloc_huge(size_t size);
  void kfree(void *ptr);
  void free_thread_allocations(uint64_t thread_id);
  size_t get_thread_allocated_size(uint64_t thread_id) const;
  void *krealloc(void *ptr, size_t size);
  void *kcalloc(size_t num, size_t size);
  void *kzalloc(size_t size);
  size_t get_total_size() const;
  size_t get_used_size() const;
  size_t get_free_size() const;
  size_t get_peak_used_size() const;
  bool validate() const;
  SystemMemoryInfo get_system_memory_info(bool recheck = false) const;
};
extern KernelHeap g_kheap;
} // namespace Kernel::Memory
extern "C" {
void *kmalloc(size_t size);
void kfree(void *ptr);
void *krealloc(void *ptr, size_t size);
void *kcalloc(size_t num, size_t size);
void *kzalloc(size_t size);
void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t num, size_t size);
}
