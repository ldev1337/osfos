#include <kernel/memory/heap.h>
#include <arch/x86_64/string.h>
#include <arch/x86_64/spinlock.h>

namespace Kernel::Memory {

KernelHeap g_kheap;
static Spinlock heap_lock;

void KernelHeap::init(void* heap_start_addr, size_t initial_size) {
    start_node = (HeapNode*)heap_start_addr;
    total_heap_size = initial_size;
    start_node->size = initial_size - sizeof(HeapNode);
    start_node->is_free = true;
    start_node->next = nullptr;
}

void* KernelHeap::kmalloc(size_t size) {
    const uint64_t flags = heap_lock.lock_irqsave();
    if (size == 0) {
        heap_lock.unlock_irqrestore(flags);
        return nullptr;
    }
    size = (size + 7) & ~7;
    HeapNode* current = start_node;
    while (current != nullptr) {
        if (current->is_free && current->size >= size) {
            if (current->size >= size + sizeof(HeapNode) + 8) {
                HeapNode* new_node = (HeapNode*)((size_t)current + sizeof(HeapNode) + size);
                new_node->size = current->size - size - sizeof(HeapNode);
                new_node->is_free = true;
                new_node->next = current->next;

                current->size = size;
                current->next = new_node;
            }
            current->is_free = false;
            void* result = (void*)((size_t)current + sizeof(HeapNode));
            heap_lock.unlock_irqrestore(flags);
            return result;
        }
        current = current->next;
    }
    heap_lock.unlock_irqrestore(flags);
    return nullptr;
}

void KernelHeap::kfree(void* ptr) {
    if (!ptr) return;
    const uint64_t flags = heap_lock.lock_irqsave();
    HeapNode* node = (HeapNode*)((size_t)ptr - sizeof(HeapNode));
    node->is_free = true;

    HeapNode* current = start_node;
    while (current != nullptr) {
        while (current->is_free && current->next && current->next->is_free) {
            current->size += sizeof(HeapNode) + current->next->size;
            current->next = current->next->next;
        }
        current = current->next;
    }
    heap_lock.unlock_irqrestore(flags);
}

void* KernelHeap::krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return nullptr;
    }

    HeapNode* node = (HeapNode*)((size_t)ptr - sizeof(HeapNode));
    size_t old_size = node->size;

    if (old_size >= size) {
        return ptr;
    }

    void* new_ptr = kmalloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, old_size);
        kfree(ptr);
    }
    return new_ptr;
}

void* KernelHeap::kcalloc(size_t num, size_t size) {
    if (size != 0 && num > SIZE_MAX / size)
        return nullptr;
    size_t total = num * size;
    void* ptr = kmalloc(total);
    if (ptr)
        memset(ptr, 0, total);

    return ptr;
}

void* KernelHeap::kzalloc(size_t size) {
    return kcalloc(1, size);
}

size_t KernelHeap::get_used_size() const {
    size_t used = 0;
    HeapNode* current = start_node;
    while (current != nullptr) {
        if (!current->is_free) {
            used += current->size + sizeof(HeapNode);
        }
        current = current->next;
    }
    return used;
}

size_t KernelHeap::get_free_size() const {
    size_t free_bytes = 0;
    HeapNode* current = start_node;
    while (current != nullptr) {
        if (current->is_free) {
            free_bytes += current->size;
        }
        current = current->next;
    }
    return free_bytes;
}

bool KernelHeap::validate() const {
    HeapNode* current = start_node;
    while (current != nullptr) {
        if (current->next && (size_t)current >= (size_t)current->next) {
            return false;
        }
        current = current->next;
    }
    return true;
}

}

extern "C" {
    void* kmalloc(size_t size) { return Kernel::Memory::g_kheap.kmalloc(size); }
    void  kfree(void* ptr) { Kernel::Memory::g_kheap.kfree(ptr); }
    void* krealloc(void* ptr, size_t size) { return Kernel::Memory::g_kheap.krealloc(ptr, size); }
    void* kcalloc(size_t num, size_t size) { return Kernel::Memory::g_kheap.kcalloc(num, size); }
    void* kzalloc(size_t size) { return Kernel::Memory::g_kheap.kzalloc(size); }
    void* malloc(size_t size) { return Kernel::Memory::g_kheap.kmalloc(size); }
    void  free(void* ptr) { Kernel::Memory::g_kheap.kfree(ptr); }
    void* realloc(void* ptr, size_t size) { return Kernel::Memory::g_kheap.krealloc(ptr, size); }
    void* calloc(size_t num, size_t size) { return Kernel::Memory::g_kheap.kcalloc(num, size); }
}

void* operator new(size_t size) {
    return malloc(size);
}

void* operator new[](size_t size) {
    return malloc(size);
}

void operator delete(void* ptr) noexcept {
    free(ptr);
}

void operator delete[](void* ptr) noexcept {
    free(ptr);
}

void operator delete(void* ptr, size_t size) noexcept {
    (void)size;
    free(ptr);
}
