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
  HeapNode *start_node;
  size_t total_heap_size;
  size_t used_bytes_count; // O(1) tracking counter
  size_t peak_used_size;

public:
  void init(void *heap_start_addr, size_t initial_size);

  void *kmalloc(size_t size);
  void kfree(void *ptr);
  void free_thread_allocations(uint64_t thread_id);
  size_t get_thread_allocated_size(uint64_t thread_id) const;
  void *krealloc(void *ptr, size_t size);
  void *kcalloc(size_t num, size_t size);
  void *kzalloc(size_t size);

  // Fast O(1) Heap Analytics
  size_t get_total_size() const { return total_heap_size; }
  size_t get_used_size() const { return used_bytes_count; }
  size_t get_free_size() const { return total_heap_size - used_bytes_count; }
  size_t get_peak_used_size() const { return peak_used_size; }
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