#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Kernel::Memory {

struct HeapNode {
    size_t size;          
    bool is_free;         
    HeapNode* next;       
};

class KernelHeap {
private:
    HeapNode* start_node;
    size_t total_heap_size;

public:
    
    void init(void* heap_start_addr, size_t initial_size);
    
    
    void* kmalloc(size_t size);
    void  kfree(void* ptr);
    void* krealloc(void* ptr, size_t size);
    void* kcalloc(size_t num, size_t size);
    void* kzalloc(size_t size);

    
    size_t get_total_size() const { return total_heap_size; }
    size_t get_used_size() const;
    size_t get_free_size() const;
    bool   validate() const;
};

extern KernelHeap g_kheap;

} 


extern "C" {
    void* kmalloc(size_t size);
    void  kfree(void* ptr);
    void* krealloc(void* ptr, size_t size);
    void* kcalloc(size_t num, size_t size);
    void* kzalloc(size_t size);

    void* malloc(size_t size);
    void  free(void* ptr);
    void* realloc(void* ptr, size_t size);
    void* calloc(size_t num, size_t size);
}