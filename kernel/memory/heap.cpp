#include <arch/x86_64/spinlock.h>
#include <arch/x86_64/string.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/pmm.h>
#include <kernel/scheduler.h>

#include <algorithm>

namespace Kernel::Memory {

KernelHeap g_kheap;
static Spinlock heap_lock;

void KernelHeap::init(void *heap_start_addr, size_t initial_size) {
  const uint64_t flags = heap_lock.lock_irqsave();
  start_node = static_cast<HeapNode *>(heap_start_addr);
  total_heap_size = initial_size;
  start_node->size = initial_size - sizeof(HeapNode);
  start_node->is_free = true;
  start_node->owner_thread_id = 0;
  start_node->next = nullptr;

  used_bytes_count =
      sizeof(HeapNode); // Initial sentinel block metadata overhead
  peak_used_size = used_bytes_count;
  heap_lock.unlock_irqrestore(flags);
}

void *KernelHeap::kmalloc(size_t size) {
  const uint64_t flags = heap_lock.lock_irqsave();
  if (size == 0) {
    heap_lock.unlock_irqrestore(flags);
    return nullptr;
  }
  size = (size + 7) & ~7;
  HeapNode *current = start_node;
  while (current != nullptr) {
    if (current->is_free && current->size >= size) {
      if (current->size >= size + sizeof(HeapNode) + 8) {
        HeapNode *new_node =
            (HeapNode *)((size_t)current + sizeof(HeapNode) + size);
        new_node->size = current->size - size - sizeof(HeapNode);
        new_node->is_free = true;
        new_node->owner_thread_id = 0;
        new_node->next = current->next;

        current->size = size;
        current->next = new_node;

        // Extra header created
        used_bytes_count += sizeof(HeapNode);
      }
      current->is_free = false;
      current->owner_thread_id = Kernel::Scheduler::current_thread_id();
      used_bytes_count += current->size;

      peak_used_size = std::max(used_bytes_count, peak_used_size);

      void *result = (void *)((size_t)current + sizeof(HeapNode));
      heap_lock.unlock_irqrestore(flags);
      return result;
    }
    current = current->next;
  }
  heap_lock.unlock_irqrestore(flags);
  return nullptr;
}

void KernelHeap::kfree(void *ptr) {
  if (!ptr)
    return;
  const uint64_t flags = heap_lock.lock_irqsave();
  HeapNode *node = (HeapNode *)((size_t)ptr - sizeof(HeapNode));
  if (!node->is_free) {
    node->is_free = true;
    node->owner_thread_id = 0;
    if (used_bytes_count >= node->size) {
      used_bytes_count -= node->size;
    }
  }

  HeapNode *current = start_node;
  while (current != nullptr) {
    while (current->is_free && current->next && current->next->is_free) {
      current->size += sizeof(HeapNode) + current->next->size;
      current->next = current->next->next;
      // Merging blocks reclaims one HeapNode header space
      if (used_bytes_count >= sizeof(HeapNode)) {
        used_bytes_count -= sizeof(HeapNode);
      }
    }
    current = current->next;
  }
  heap_lock.unlock_irqrestore(flags);
}

void KernelHeap::free_thread_allocations(uint64_t thread_id) {
  if (!thread_id)
    return;

  const uint64_t flags = heap_lock.lock_irqsave();
  for (HeapNode *node = start_node; node; node = node->next) {
    if (!node->is_free && node->owner_thread_id == thread_id) {
      node->is_free = true;
      node->owner_thread_id = 0;
      if (used_bytes_count >= node->size)
        used_bytes_count -= node->size;
    }
  }

  for (HeapNode *node = start_node; node; node = node->next) {
    while (node->is_free && node->next && node->next->is_free) {
      node->size += sizeof(HeapNode) + node->next->size;
      node->next = node->next->next;
      if (used_bytes_count >= sizeof(HeapNode))
        used_bytes_count -= sizeof(HeapNode);
    }
  }
  heap_lock.unlock_irqrestore(flags);
}

size_t KernelHeap::get_thread_allocated_size(uint64_t thread_id) const {
  if (!thread_id)
    return 0;

  size_t allocated_size = 0;
  const uint64_t flags = heap_lock.lock_irqsave();
  for (HeapNode *node = start_node; node; node = node->next) {
    if (!node->is_free && node->owner_thread_id == thread_id)
      allocated_size += node->size;
  }
  heap_lock.unlock_irqrestore(flags);
  return allocated_size;
}

void *KernelHeap::krealloc(void *ptr, size_t size) {
  if (!ptr)
    return kmalloc(size);
  if (size == 0) {
    kfree(ptr);
    return nullptr;
  }

  HeapNode *node = (HeapNode *)((size_t)ptr - sizeof(HeapNode));
  size_t old_size = node->size;

  if (old_size >= size) {
    return ptr;
  }

  void *new_ptr = kmalloc(size);
  if (new_ptr) {
    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);
  }
  return new_ptr;
}

void *KernelHeap::kcalloc(size_t num, size_t size) {
  if (size != 0 && num > SIZE_MAX / size)
    return nullptr;
  size_t total = num * size;
  void *ptr = kmalloc(total);
  if (ptr)
    memset(ptr, 0, total);

  return ptr;
}

void *KernelHeap::kzalloc(size_t size) { return kcalloc(1, size); }

bool KernelHeap::validate() const {
  const uint64_t flags = heap_lock.lock_irqsave();
  HeapNode *current = start_node;
  while (current != nullptr) {
    if ((current->next != nullptr) &&
        (size_t)current >= (size_t)current->next) {
      heap_lock.unlock_irqrestore(flags);
      return false;
    }
    current = current->next;
  }
  heap_lock.unlock_irqrestore(flags);
  return true;
}

SystemMemoryInfo KernelHeap::get_system_memory_info(bool recheck) const {
  SystemMemoryInfo info;
  info.pmm_total_memory = g_pmm.get_total_memory();
  if (recheck) {
    info.pmm_used_memory = g_pmm.recheck_used_memory();
    info.pmm_free_memory = info.pmm_total_memory - info.pmm_used_memory;
  } else {
    info.pmm_used_memory = g_pmm.get_used_memory();
    info.pmm_free_memory = g_pmm.get_free_memory();
  }

  const uint64_t flags = heap_lock.lock_irqsave();
  info.heap_total_size = total_heap_size;
  info.heap_used_size = used_bytes_count;
  if (recheck) {
    size_t block_count = 0;
    size_t allocated_bytes = 0;
    for (HeapNode *node = start_node; node != nullptr; node = node->next) {
      ++block_count;
      if (!node->is_free)
        allocated_bytes += node->size;
    }
    info.heap_used_size = allocated_bytes + block_count * sizeof(HeapNode);
  }
  info.heap_free_size = total_heap_size - info.heap_used_size;
  info.heap_peak_size = peak_used_size;
  heap_lock.unlock_irqrestore(flags);

  return info;
}

} // namespace Kernel::Memory

extern "C" {
void *kmalloc(size_t size) { return Kernel::Memory::g_kheap.kmalloc(size); }
void kfree(void *ptr) { Kernel::Memory::g_kheap.kfree(ptr); }
void *krealloc(void *ptr, size_t size) {
  return Kernel::Memory::g_kheap.krealloc(ptr, size);
}
void *kcalloc(size_t num, size_t size) {
  return Kernel::Memory::g_kheap.kcalloc(num, size);
}
void *kzalloc(size_t size) { return Kernel::Memory::g_kheap.kzalloc(size); }
void *malloc(size_t size) { return Kernel::Memory::g_kheap.kmalloc(size); }
void free(void *ptr) { Kernel::Memory::g_kheap.kfree(ptr); }
void *realloc(void *ptr, size_t size) {
  return Kernel::Memory::g_kheap.krealloc(ptr, size);
}
void *calloc(size_t num, size_t size) {
  return Kernel::Memory::g_kheap.kcalloc(num, size);
}
}

void *operator new(size_t size) { return malloc(size); }

void *operator new[](size_t size) { return malloc(size); }

void operator delete(void *ptr) noexcept { free(ptr); }

void operator delete[](void *ptr) noexcept { free(ptr); }

void operator delete(void *ptr, size_t size) noexcept {
  (void)size;
  free(ptr);
}