#include <algorithm>
#include <arch/x86_64/spinlock.h>
#include <arch/x86_64/string.h>
#include <cstdint>
#include <kernel/memory/heap.h>
#include <kernel/memory/pmm.h>
#include <kernel/scheduler.h>

namespace Kernel::Memory {

KernelHeap g_kheap;
static Spinlock heap_lock;

namespace {

constexpr size_t ALIGNMENT = 16;
constexpr size_t SLAB_SIZE = 4096;
constexpr size_t SLAB_MASK = SLAB_SIZE - 1;
constexpr unsigned FL_COUNT = 64;
constexpr unsigned SL_BITS = 3;
constexpr unsigned SL_COUNT = 1U << SL_BITS;
constexpr unsigned CLASS_COUNT = 12;
constexpr size_t CLASS_SIZE[CLASS_COUNT] = {16,  32,  48,  64,  96,  128,
                                            192, 256, 384, 512, 768, 1024};

constexpr size_t BLOCK_USED = 1;
constexpr size_t BLOCK_SLAB = 2;
constexpr size_t BLOCK_FLAGS = 0xF;
constexpr size_t MIN_BLOCK_SIZE = 32; // Optimized: reduced from 64 to 32 to
                                      // minimize tiny-allocation fragmentation

constexpr uint32_t TAG_FREE = 0x46524545U;  // "FREE"
constexpr uint32_t TAG_LARGE = 0x4C415247U; // "LARG"
constexpr uint32_t TAG_SLAB = 0x534C0000U;  // "SL.."
constexpr uint32_t TAG_SLAB_MASK = 0xFFFF0000U;
constexpr uint64_t SLAB_MAGIC = 0x534C414250414745ULL; // "SLABPAGE"
constexpr uint64_t HUGE_MAGIC = 0x48554745ULL;         // "HUGE"

// Unified Block structure:
// Uses a union to eliminate the separate AllocHeader for large allocations.
// - Free state: uses free_prev and free_next for TLSF segregated bins.
// - Allocated state: repurposes the same space for owner, requested size, and
// tag.
struct alignas(16) Block {
  size_t size_flags;
  size_t prev_size;
  union {
    struct {
      Block *free_prev;
      Block *free_next;
    };
    struct {
      uint64_t owner;
      uint32_t requested;
      uint32_t tag;
    } alloc;
  };
};

struct AllocHeader {
  uint64_t owner;
  uint32_t requested;
  uint32_t tag;
};

struct alignas(16) SlabPage {
  uint64_t magic;
  SlabPage *all_next;
  SlabPage *all_prev;
  SlabPage *free_next;
  SlabPage *free_prev;
  void *free_head;
  void *backing;
  uint32_t stride;
  uint16_t capacity;
  uint16_t free_count;
  uint16_t used_count;
  uint8_t class_index;
  uint8_t on_free_list;
  uint8_t reserved[4];
};

struct HugeAllocHeader {
  uint64_t magic;
  size_t size;
};

static_assert(sizeof(Block) == 32);
static_assert(sizeof(AllocHeader) == 16);
static_assert(sizeof(SlabPage) == 80);

static inline size_t block_size(const Block *b) {
  return b->size_flags & ~BLOCK_FLAGS;
}

static inline bool block_used(const Block *b) {
  return (b->size_flags & BLOCK_USED) != 0;
}

static inline bool block_slab(const Block *b) {
  return (b->size_flags & BLOCK_SLAB) != 0;
}

static inline size_t align_up_size(size_t n, size_t a) {
  return (n + a - 1) & ~(a - 1);
}

static inline uintptr_t align_up_ptr(uintptr_t n, size_t a) {
  return (n + a - 1) & ~(uintptr_t)(a - 1);
}

static inline unsigned msb_index(size_t n) {
  return 63u - (unsigned)__builtin_clzll((unsigned long long)n);
}

static bool mapping_insert(size_t size, unsigned &fl, unsigned &sl) {
  if (size < MIN_BLOCK_SIZE)
    return false;
  fl = msb_index(size);
  if (fl < SL_BITS || fl >= FL_COUNT)
    return false;
  const unsigned shift = fl - SL_BITS;
  sl = (unsigned)(size >> shift) - SL_COUNT;
  return sl < SL_COUNT;
}

static bool mapping_search(size_t size, unsigned &fl, unsigned &sl) {
  size = std::max(size, MIN_BLOCK_SIZE);
  const unsigned f = msb_index(size);
  if (f < SL_BITS || f >= FL_COUNT)
    return false;
  const size_t step = static_cast<size_t>(1) << (f - SL_BITS);
  if (size > static_cast<size_t>(-1) - (step - 1))
    return false;
  const size_t rounded = (size + step - 1) & ~(step - 1);
  return mapping_insert(rounded, fl, sl);
}

static unsigned class_for_size(size_t size) {
  unsigned lo = 0, hi = CLASS_COUNT;
  while (lo < hi) {
    const unsigned mid = (lo + hi) >> 1;
    if (CLASS_SIZE[mid] < size)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

} // namespace

void KernelHeap::insert_free_block(void *raw) {
  Block *b = static_cast<Block *>(raw);
  unsigned fl, sl;
  if (!mapping_insert(block_size(b), fl, sl))
    return;
  Block *head = static_cast<Block *>(free_bins[fl][sl]);
  b->free_prev = nullptr;
  b->free_next = head;
  if (head)
    head->free_prev = b;
  free_bins[fl][sl] = b;
  sl_bitmap[fl] |= static_cast<uint8_t>(1u << sl);
  fl_bitmap |= static_cast<uint64_t>(1) << fl;
}

void KernelHeap::remove_free_block(void *raw) {
  Block *b = static_cast<Block *>(raw);
  unsigned fl, sl;
  if (!mapping_insert(block_size(b), fl, sl))
    return;
  if (b->free_prev)
    b->free_prev->free_next = b->free_next;
  else
    free_bins[fl][sl] = b->free_next;
  if (b->free_next)
    b->free_next->free_prev = b->free_prev;
  b->free_prev = b->free_next = nullptr;
  if (!free_bins[fl][sl]) {
    sl_bitmap[fl] &= static_cast<uint8_t>(~(1u << sl));
    if (!sl_bitmap[fl])
      fl_bitmap &= ~(static_cast<uint64_t>(1) << fl);
  }
}

void *KernelHeap::find_suitable_block(size_t size) {
  unsigned fl, sl;
  if (!mapping_search(size, fl, sl))
    return nullptr;
  uint8_t available = sl_bitmap[fl] & static_cast<uint8_t>(0xFFu << sl);
  if (available) {
    sl = static_cast<unsigned>(__builtin_ctz(static_cast<unsigned>(available)));
    return free_bins[fl][sl];
  }
  if (fl == FL_COUNT - 1)
    return nullptr;
  const uint64_t higher = fl_bitmap & (~static_cast<uint64_t>(0) << (fl + 1));
  if (!higher)
    return nullptr;
  fl = static_cast<unsigned>(__builtin_ctzll(higher));
  if (!sl_bitmap[fl])
    return nullptr;
  sl = static_cast<unsigned>(
      __builtin_ctz(static_cast<unsigned>(sl_bitmap[fl])));
  return free_bins[fl][sl];
}

void *KernelHeap::allocate_block(size_t payload_size, size_t alignment,
                                 uint32_t flags, void **block_out) {
  if (!first_block ||
      payload_size > (size_t)-1 - sizeof(Block) - (ALIGNMENT - 1))
    return nullptr;
  alignment = std::max(alignment, ALIGNMENT);
  if ((alignment & (alignment - 1)) != 0)
    return nullptr;
  size_t needed = align_up_size(sizeof(Block) + payload_size, ALIGNMENT);
  size_t search_size = needed;
  if (alignment > ALIGNMENT) {
    const size_t extra = alignment - ALIGNMENT;
    if (search_size > static_cast<size_t>(-1) - extra)
      return nullptr;
    search_size += extra;
  }
  Block *b = static_cast<Block *>(find_suitable_block(search_size));
  if (!b)
    return nullptr;
  remove_free_block(b);
  const size_t original_size = block_size(b);
  const uintptr_t payload_base = (uintptr_t)b + sizeof(Block);
  if (payload_base > static_cast<uintptr_t>(-1) - (alignment - 1)) {
    insert_free_block(b);
    return nullptr;
  }
  const uintptr_t aligned_payload = align_up_ptr(payload_base, alignment);
  const size_t prefix = static_cast<size_t>(aligned_payload - payload_base);
  const size_t free_prefix = prefix >= MIN_BLOCK_SIZE ? prefix : 0;
  const size_t inside_prefix = prefix - free_prefix;
  if (needed > original_size - free_prefix ||
      inside_prefix > original_size - free_prefix - needed) {
    insert_free_block(b);
    return nullptr;
  }
  Block *allocated = b;
  size_t allocated_needed = needed + inside_prefix;
  if (free_prefix) {
    b->size_flags = free_prefix;
    allocated = reinterpret_cast<Block *>(aligned_payload - sizeof(Block));
    allocated->prev_size = free_prefix;
    insert_free_block(b);
  }
  size_t suffix = original_size - free_prefix - allocated_needed;
  size_t allocated_total = allocated_needed;
  if (suffix >= MIN_BLOCK_SIZE) {
    Block *tail =
        reinterpret_cast<Block *>((uintptr_t)allocated + allocated_needed);
    tail->prev_size = allocated_needed;
    tail->size_flags = suffix;
    tail->free_prev = tail->free_next = nullptr;
    Block *after = reinterpret_cast<Block *>((uintptr_t)tail + suffix);
    if ((uintptr_t)after < (uintptr_t)heap_end)
      after->prev_size = suffix;
    insert_free_block(tail);
  } else {
    allocated_total += suffix;
    Block *after =
        reinterpret_cast<Block *>((uintptr_t)allocated + allocated_total);
    if ((uintptr_t)after < (uintptr_t)heap_end)
      after->prev_size = allocated_total;
  }
  allocated->size_flags = allocated_total | BLOCK_USED | (flags & BLOCK_SLAB);
  used_bytes_count += allocated_total;
  if (used_bytes_count > peak_used_size)
    peak_used_size = used_bytes_count;
  if (block_out)
    *block_out = allocated;
  return reinterpret_cast<void *>(aligned_payload);
}

void *KernelHeap::release_block(void *raw) {
  Block *b = static_cast<Block *>(raw);
  size_t size = block_size(b);
  used_bytes_count -= size;
  b->size_flags = size;
  uintptr_t next_addr = (uintptr_t)b + size;
  if (next_addr < (uintptr_t)heap_end) {
    Block *next = reinterpret_cast<Block *>(next_addr);
    if (!block_used(next)) {
      remove_free_block(next);
      size += block_size(next);
      b->size_flags = size;
    }
  }
  if (b->prev_size) {
    Block *prev = reinterpret_cast<Block *>((uintptr_t)b - b->prev_size);
    if (!block_used(prev)) {
      remove_free_block(prev);
      size += block_size(prev);
      prev->size_flags = size;
      b = prev;
    }
  }
  next_addr = (uintptr_t)b + size;
  if (next_addr < (uintptr_t)heap_end)
    reinterpret_cast<Block *>(next_addr)->prev_size = size;
  insert_free_block(b);
  return b;
}

void KernelHeap::add_slab_free_page(void *raw) {
  SlabPage *page = static_cast<SlabPage *>(raw);
  if (page->on_free_list)
    return;
  const unsigned idx = page->class_index;
  SlabPage *head = static_cast<SlabPage *>(slab_free[idx]);
  page->free_prev = nullptr;
  page->free_next = head;
  if (head)
    head->free_prev = page;
  slab_free[idx] = page;
  page->on_free_list = 1;
}

void KernelHeap::remove_slab_free_page(void *raw) {
  SlabPage *page = static_cast<SlabPage *>(raw);
  if (!page->on_free_list)
    return;
  const unsigned idx = page->class_index;
  if (page->free_prev)
    page->free_prev->free_next = page->free_next;
  else
    slab_free[idx] = page->free_next;
  if (page->free_next)
    page->free_next->free_prev = page->free_prev;
  page->free_prev = page->free_next = nullptr;
  page->on_free_list = 0;
}

void KernelHeap::unlink_slab_page(void *raw) {
  SlabPage *page = static_cast<SlabPage *>(raw);
  if (page->all_prev)
    page->all_prev->all_next = page->all_next;
  else
    slab_pages = page->all_next;
  if (page->all_next)
    page->all_next->all_prev = page->all_prev;
  page->all_prev = page->all_next = nullptr;
}

void KernelHeap::release_slab_page(void *raw) {
  SlabPage *page = static_cast<SlabPage *>(raw);
  void *backing = page->backing;
  remove_slab_free_page(page);
  unlink_slab_page(page);
  page->magic = 0;
  release_block(backing);
}

void *KernelHeap::allocate_slab_locked(unsigned class_index, size_t size,
                                       uint64_t owner) {
  SlabPage *page = static_cast<SlabPage *>(slab_free[class_index]);
  if (!page) {
    void *backing = nullptr;
    void *memory =
        allocate_block(SLAB_SIZE, SLAB_SIZE, (uint32_t)BLOCK_SLAB, &backing);
    if (!memory)
      return nullptr;
    page = static_cast<SlabPage *>(memory);
    memset(page, 0, sizeof(SlabPage));
    page->magic = SLAB_MAGIC;
    page->backing = backing;
    page->class_index = (uint8_t)class_index;
    page->stride = (uint32_t)align_up_size(
        sizeof(AllocHeader) + CLASS_SIZE[class_index], ALIGNMENT);
    page->capacity = (uint16_t)((SLAB_SIZE - sizeof(SlabPage)) / page->stride);
    page->free_count = page->capacity;
    page->used_count = 0;
    const size_t slot_base = sizeof(SlabPage);
    for (unsigned i = 0; i < page->capacity; ++i) {
      AllocHeader *header = reinterpret_cast<AllocHeader *>(
          (uintptr_t)page + slot_base + (size_t)i * page->stride);
      header->owner = 0;
      header->requested = 0;
      header->tag = TAG_FREE;
      void *user = header + 1;
      *reinterpret_cast<void **>(user) = page->free_head;
      page->free_head = user;
    }
    page->all_next = static_cast<SlabPage *>(slab_pages);
    page->all_prev = nullptr;
    if (page->all_next)
      page->all_next->all_prev = page;
    slab_pages = page;
    add_slab_free_page(page);
  }
  void *user = page->free_head;
  page->free_head = *reinterpret_cast<void **>(user);
  AllocHeader *header = reinterpret_cast<AllocHeader *>(user) - 1;
  header->owner = owner;
  header->requested = (uint32_t)size;
  header->tag = TAG_SLAB | class_index;
  --page->free_count;
  ++page->used_count;
  if (!page->free_count)
    remove_slab_free_page(page);
  return user;
}

void *KernelHeap::allocate_locked(size_t size, uint64_t owner) {
  if (!size || size > UINT32_MAX)
    return nullptr;
  if (size <= CLASS_SIZE[CLASS_COUNT - 1]) {
    const unsigned idx = class_for_size(size);
    if (idx < CLASS_COUNT)
      return allocate_slab_locked(idx, size, owner);
  }
  if (size > (size_t)-1 - sizeof(Block) - (ALIGNMENT - 1))
    return nullptr;
  const size_t rounded = align_up_size(size, ALIGNMENT);
  void *block_raw = nullptr;
  void *payload = allocate_block(rounded, ALIGNMENT, 0, &block_raw);
  if (!payload)
    return nullptr;
  Block *block = static_cast<Block *>(block_raw);
  block->alloc.owner = owner;
  block->alloc.requested = (uint32_t)size;
  block->alloc.tag = TAG_LARGE;
  return payload;
}

void KernelHeap::free_slab_locked(void *ptr, void *raw_header, void *raw_page) {
  AllocHeader *header = static_cast<AllocHeader *>(raw_header);
  SlabPage *page = static_cast<SlabPage *>(raw_page);
  const bool was_full = page->free_count == 0;
  header->owner = 0;
  header->requested = 0;
  header->tag = TAG_FREE;
  *reinterpret_cast<void **>(ptr) = page->free_head;
  page->free_head = ptr;
  ++page->free_count;
  --page->used_count;
  if (was_full)
    add_slab_free_page(page);
  if (!page->used_count)
    release_slab_page(page);
}

void KernelHeap::free_locked(void *ptr) {
  if (!ptr)
    return;
  Block *block = reinterpret_cast<Block *>(ptr) - 1;
  if (block_slab(block)) {
    SlabPage *page =
        reinterpret_cast<SlabPage *>((uintptr_t)ptr & ~(uintptr_t)SLAB_MASK);
    AllocHeader *header = static_cast<AllocHeader *>(ptr) - 1;
    free_slab_locked(ptr, header, page);
  } else if (block->alloc.tag == TAG_LARGE) {
    block->alloc.owner = 0;
    block->alloc.requested = 0;
    block->alloc.tag = TAG_FREE;
    release_block(block);
  }
}

bool KernelHeap::resize_large_locked(void *raw_block, size_t size) {
  if (!size || size > UINT32_MAX)
    return false;
  Block *block = static_cast<Block *>(raw_block);
  if (size > (size_t)-1 - sizeof(Block) - (ALIGNMENT - 1))
    return false;
  const size_t needed = sizeof(Block) + align_up_size(size, ALIGNMENT);
  const size_t old_total = block_size(block);
  if (needed <= old_total) {
    const size_t remainder = old_total - needed;
    if (remainder >= MIN_BLOCK_SIZE) {
      block->size_flags = needed | BLOCK_USED;
      Block *tail = reinterpret_cast<Block *>((uintptr_t)block + needed);
      tail->prev_size = needed;
      tail->size_flags = remainder;
      tail->free_prev = tail->free_next = nullptr;
      used_bytes_count -= remainder;
      uintptr_t after_addr = (uintptr_t)tail + remainder;
      if (after_addr < (uintptr_t)heap_end) {
        Block *after = reinterpret_cast<Block *>(after_addr);
        if (!block_used(after)) {
          remove_free_block(after);
          tail->size_flags = block_size(tail) + block_size(after);
        }
      }
      const size_t tail_size = block_size(tail);
      after_addr = (uintptr_t)tail + tail_size;
      if (after_addr < (uintptr_t)heap_end)
        reinterpret_cast<Block *>(after_addr)->prev_size = tail_size;
      insert_free_block(tail);
    }
    block->alloc.requested = (uint32_t)size;
    return true;
  }
  uintptr_t next_addr = (uintptr_t)block + old_total;
  if (next_addr < (uintptr_t)heap_end) {
    Block *next = reinterpret_cast<Block *>(next_addr);
    if (!block_used(next)) {
      const size_t next_size = block_size(next);
      const size_t combined = old_total + next_size;
      if (combined >= needed) {
        remove_free_block(next);
        const size_t remainder = combined - needed;
        if (remainder >= MIN_BLOCK_SIZE) {
          block->size_flags = needed | BLOCK_USED;
          Block *tail = reinterpret_cast<Block *>((uintptr_t)block + needed);
          tail->prev_size = needed;
          tail->size_flags = remainder;
          tail->free_prev = tail->free_next = nullptr;
          uintptr_t after_addr = (uintptr_t)tail + remainder;
          if (after_addr < (uintptr_t)heap_end)
            reinterpret_cast<Block *>(after_addr)->prev_size = remainder;
          insert_free_block(tail);
          used_bytes_count += needed - old_total;
        } else {
          block->size_flags = combined | BLOCK_USED;
          uintptr_t after_addr = (uintptr_t)block + combined;
          if (after_addr < (uintptr_t)heap_end)
            reinterpret_cast<Block *>(after_addr)->prev_size = combined;
          used_bytes_count += next_size;
        }
        block->alloc.requested = (uint32_t)size;
        if (used_bytes_count > peak_used_size)
          peak_used_size = used_bytes_count;
        return true;
      }
    }
  }
  return false;
}

void KernelHeap::init(void *heap_start_addr, size_t initial_size) {
  const uint64_t flags = heap_lock.lock_irqsave();
  heap_begin = heap_end = first_block = slab_pages = nullptr;
  fl_bitmap = 0;
  used_bytes_count = peak_used_size = total_heap_size = 0;
  for (unsigned fl = 0; fl < FL_COUNT; ++fl) {
    sl_bitmap[fl] = 0;
    for (unsigned sl = 0; sl < SL_COUNT; ++sl)
      free_bins[fl][sl] = nullptr;
  }
  for (unsigned i = 0; i < CLASS_COUNT; ++i)
    slab_free[i] = nullptr;
  const uintptr_t raw = (uintptr_t)heap_start_addr;
  if (heap_start_addr && initial_size &&
      raw <= (uintptr_t)-1 - (ALIGNMENT - 1) &&
      initial_size <= (uintptr_t)-1 - raw) {
    const uintptr_t begin = align_up_ptr(raw, ALIGNMENT);
    const uintptr_t end = (raw + initial_size) & ~(uintptr_t)(ALIGNMENT - 1);
    if (end > begin && end - begin >= MIN_BLOCK_SIZE) {
      heap_begin = reinterpret_cast<void *>(begin);
      heap_end = reinterpret_cast<void *>(end);
      total_heap_size = end - begin;
      first_block = reinterpret_cast<void *>(begin);
      Block *first = static_cast<Block *>(first_block);
      first->size_flags = total_heap_size;
      first->prev_size = 0;
      first->free_prev = first->free_next = nullptr;
      insert_free_block(first);
    }
  }
  heap_lock.unlock_irqrestore(flags);
}

void *KernelHeap::kmalloc_huge(size_t size) {
  constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024; // 2 MiB
  size_t total_needed = sizeof(HugeAllocHeader) + size;
  size_t num_huge_pages = (total_needed + HUGE_PAGE_SIZE - 1) / HUGE_PAGE_SIZE;

  void *phys_addr = nullptr;
  for (size_t i = 0; i < num_huge_pages; ++i) {
    void *p = g_pmm.allocate_huge_page();
    if (!p) {
      if (i > 0) {
        g_pmm.free_page_block(phys_addr, i * 512);
      }
      return nullptr;
    }
    if (i == 0)
      phys_addr = p;
  }

  uintptr_t virt_addr = reinterpret_cast<uintptr_t>(phys_addr);
  HugeAllocHeader *header = reinterpret_cast<HugeAllocHeader *>(virt_addr);
  header->magic = HUGE_MAGIC;
  header->size = num_huge_pages * HUGE_PAGE_SIZE;

  return reinterpret_cast<void *>(header + 1);
}

void *KernelHeap::kmalloc(size_t size) {
  if (!size)
    return nullptr;
  if (size >= 2 * 1024 * 1024) {
    return kmalloc_huge(size);
  }
  const uint64_t flags = heap_lock.lock_irqsave();
  void *ptr = allocate_locked(size, Kernel::Scheduler::current_thread_id());
  heap_lock.unlock_irqrestore(flags);
  return ptr;
}

void KernelHeap::kfree(void *ptr) {
  if (!ptr)
    return;
  HugeAllocHeader *huge_header = reinterpret_cast<HugeAllocHeader *>(ptr) - 1;
  if (huge_header->magic == HUGE_MAGIC) {
    size_t total_size = huge_header->size;
    size_t num_huge_pages = total_size / (2 * 1024 * 1024);
    void *phys_addr = reinterpret_cast<void *>(huge_header);
    g_pmm.free_page_block(phys_addr, num_huge_pages * 512);
    return;
  }
  const uint64_t flags = heap_lock.lock_irqsave();
  free_locked(ptr);
  heap_lock.unlock_irqrestore(flags);
}

void KernelHeap::free_thread_allocations(uint64_t thread_id) {
  if (!thread_id)
    return;
  const uint64_t flags = heap_lock.lock_irqsave();
  Block *block = static_cast<Block *>(first_block);
  while (block && (uintptr_t)block < (uintptr_t)heap_end) {
    if (!block_used(block)) {
      block = reinterpret_cast<Block *>((uintptr_t)block + block_size(block));
      continue;
    }
    const size_t old_size = block_size(block);
    if (!block_slab(block)) {
      if (block->alloc.tag == TAG_LARGE && block->alloc.owner == thread_id) {
        block->alloc.owner = 0;
        block->alloc.requested = 0;
        block->alloc.tag = TAG_FREE;
        Block *merged = static_cast<Block *>(release_block(block));
        block =
            reinterpret_cast<Block *>((uintptr_t)merged + block_size(merged));
        continue;
      }
    }
    block = reinterpret_cast<Block *>((uintptr_t)block + old_size);
  }
  SlabPage *page = static_cast<SlabPage *>(slab_pages);
  while (page) {
    SlabPage *next_page = page->all_next;
    const unsigned idx = page->class_index;
    for (unsigned i = 0; i < page->capacity; ++i) {
      AllocHeader *header = reinterpret_cast<AllocHeader *>(
          (uintptr_t)page + sizeof(SlabPage) + (size_t)i * page->stride);
      if (header->tag == (TAG_SLAB | idx) && header->owner == thread_id) {
        void *user = header + 1;
        const bool was_full = page->free_count == 0;
        header->owner = 0;
        header->requested = 0;
        header->tag = TAG_FREE;
        *reinterpret_cast<void **>(user) = page->free_head;
        page->free_head = user;
        ++page->free_count;
        --page->used_count;
        if (was_full)
          add_slab_free_page(page);
      }
    }
    if (!page->used_count)
      release_slab_page(page);
    page = next_page;
  }
  heap_lock.unlock_irqrestore(flags);
}

size_t KernelHeap::get_thread_allocated_size(uint64_t thread_id) const {
  if (!thread_id)
    return 0;
  const uint64_t flags = heap_lock.lock_irqsave();
  size_t total = 0;
  Block *block = static_cast<Block *>(first_block);
  while (block && (uintptr_t)block < (uintptr_t)heap_end) {
    if (block_used(block) && !block_slab(block)) {
      if (block->alloc.tag == TAG_LARGE && block->alloc.owner == thread_id)
        total += block->alloc.requested;
    }
    block = reinterpret_cast<Block *>((uintptr_t)block + block_size(block));
  }
  for (SlabPage *page = static_cast<SlabPage *>(slab_pages); page;
       page = page->all_next) {
    for (unsigned i = 0; i < page->capacity; ++i) {
      AllocHeader *header = reinterpret_cast<AllocHeader *>(
          (uintptr_t)page + sizeof(SlabPage) + (size_t)i * page->stride);
      if (header->tag == (TAG_SLAB | page->class_index) &&
          header->owner == thread_id)
        total += header->requested;
    }
  }
  heap_lock.unlock_irqrestore(flags);
  return total;
}

void *KernelHeap::krealloc(void *ptr, size_t size) {
  if (!ptr)
    return kmalloc(size);
  if (!size) {
    kfree(ptr);
    return nullptr;
  }
  if (size > UINT32_MAX)
    return nullptr;
  const uint64_t flags = heap_lock.lock_irqsave();
  Block *block = reinterpret_cast<Block *>(ptr) - 1;
  void *result = ptr;
  if (block_slab(block)) {
    SlabPage *page =
        reinterpret_cast<SlabPage *>((uintptr_t)ptr & ~(uintptr_t)SLAB_MASK);
    AllocHeader *header = static_cast<AllocHeader *>(ptr) - 1;
    if (size <= CLASS_SIZE[page->class_index]) {
      header->requested = (uint32_t)size;
    } else {
      const size_t old_size = header->requested;
      const uint64_t owner = header->owner;
      result = allocate_locked(size, owner);
      if (result) {
        memcpy(result, ptr, old_size < size ? old_size : size);
        free_slab_locked(ptr, header, page);
      }
    }
  } else if (block->alloc.tag == TAG_LARGE) {
    if (!resize_large_locked(block, size)) {
      const size_t old_size = block->alloc.requested;
      const uint64_t owner = block->alloc.owner;
      result = allocate_locked(size, owner);
      if (result) {
        memcpy(result, ptr, old_size < size ? old_size : size);
        block->alloc.owner = 0;
        block->alloc.requested = 0;
        block->alloc.tag = TAG_FREE;
        release_block(block);
      }
    }
  } else {
    result = nullptr;
  }
  heap_lock.unlock_irqrestore(flags);
  return result;
}

void *KernelHeap::kcalloc(size_t num, size_t size) {
  if (size && num > (size_t)-1 / size)
    return nullptr;
  const size_t total = num * size;
  void *ptr = kmalloc(total);
  if (ptr)
    memset(ptr, 0, total);
  return ptr;
}

void *KernelHeap::kzalloc(size_t size) { return kcalloc(1, size); }

size_t KernelHeap::get_total_size() const {
  const uint64_t flags = heap_lock.lock_irqsave();
  const size_t value = total_heap_size;
  heap_lock.unlock_irqrestore(flags);
  return value;
}

size_t KernelHeap::get_used_size() const {
  const uint64_t flags = heap_lock.lock_irqsave();
  const size_t value = used_bytes_count;
  heap_lock.unlock_irqrestore(flags);
  return value;
}

size_t KernelHeap::get_free_size() const {
  const uint64_t flags = heap_lock.lock_irqsave();
  const size_t value = used_bytes_count <= total_heap_size
                           ? total_heap_size - used_bytes_count
                           : 0;
  heap_lock.unlock_irqrestore(flags);
  return value;
}

size_t KernelHeap::get_peak_used_size() const {
  const uint64_t flags = heap_lock.lock_irqsave();
  const size_t value = peak_used_size;
  heap_lock.unlock_irqrestore(flags);
  return value;
}

bool KernelHeap::validate() const {
  const uint64_t flags = heap_lock.lock_irqsave();
  bool valid = true;
  if (!heap_begin || !heap_end || !first_block ||
      (uintptr_t)heap_begin >= (uintptr_t)heap_end)
    valid = false;
  size_t physical_used = 0, physical_free = 0, previous_size = 0;
  uintptr_t cursor = (uintptr_t)heap_begin;
  const uintptr_t end = (uintptr_t)heap_end;
  size_t block_limit = total_heap_size / MIN_BLOCK_SIZE + 1;
  while (valid && cursor < end && block_limit--) {
    Block *b = reinterpret_cast<Block *>(cursor);
    const size_t size = block_size(b);
    if ((cursor & (ALIGNMENT - 1)) || (size < MIN_BLOCK_SIZE) ||
        (size & (ALIGNMENT - 1)) || size > end - cursor ||
        b->prev_size != previous_size) {
      valid = false;
      break;
    }
    if (block_used(b))
      physical_used += size;
    else {
      ++physical_free;
      uintptr_t next = cursor + size;
      if (next < end && !block_used(reinterpret_cast<Block *>(next))) {
        valid = false;
        break;
      }
    }
    previous_size = size;
    cursor += size;
  }
  if (cursor != end || block_limit == 0 || physical_used != used_bytes_count)
    valid = false;
  uint64_t expected_fl = 0;
  size_t listed_free = 0;
  for (unsigned fl = 0; valid && fl < FL_COUNT; ++fl) {
    uint8_t expected_sl = 0;
    for (unsigned sl = 0; sl < SL_COUNT; ++sl) {
      Block *prev = nullptr;
      Block *b = static_cast<Block *>(free_bins[fl][sl]);
      size_t chain_limit = total_heap_size / MIN_BLOCK_SIZE + 1;
      while (b && chain_limit--) {
        unsigned actual_fl, actual_sl;
        if (block_used(b) || b->free_prev != prev ||
            !mapping_insert(block_size(b), actual_fl, actual_sl) ||
            actual_fl != fl || actual_sl != sl) {
          valid = false;
          break;
        }
        prev = b;
        b = b->free_next;
        ++listed_free;
      }
      if (b)
        valid = false;
      if (free_bins[fl][sl])
        expected_sl |= (uint8_t)(1u << sl);
    }
    if (expected_sl)
      expected_fl |= (uint64_t)1 << fl;
    if (expected_sl != sl_bitmap[fl])
      valid = false;
  }
  if (expected_fl != fl_bitmap || listed_free != physical_free)
    valid = false;
  size_t page_limit = total_heap_size / SLAB_SIZE + 1;
  size_t expected_free_pages[CLASS_COUNT] = {};
  SlabPage *prev_page = nullptr;
  for (SlabPage *page = static_cast<SlabPage *>(slab_pages);
       valid && page && page_limit--; page = page->all_next) {
    if (page->all_prev != prev_page || page->magic != SLAB_MAGIC ||
        page->class_index >= CLASS_COUNT ||
        page->stride !=
            align_up_size(sizeof(AllocHeader) + CLASS_SIZE[page->class_index],
                          ALIGNMENT)) {
      valid = false;
      break;
    }
    Block *backing = static_cast<Block *>(page->backing);
    if (!block_used(backing) || !block_slab(backing) ||
        (uintptr_t)page < (uintptr_t)backing + sizeof(Block) ||
        (uintptr_t)page + SLAB_SIZE >
            (uintptr_t)backing + block_size(backing)) {
      valid = false;
      break;
    }
    const unsigned capacity = (SLAB_SIZE - sizeof(SlabPage)) / page->stride;
    if (page->capacity != capacity ||
        page->free_count + page->used_count != page->capacity) {
      valid = false;
      break;
    }
    unsigned free_slots = 0, used_slots = 0;
    for (unsigned i = 0; i < page->capacity; ++i) {
      AllocHeader *h = reinterpret_cast<AllocHeader *>(
          (uintptr_t)page + sizeof(SlabPage) + (size_t)i * page->stride);
      if (h->tag == TAG_FREE)
        ++free_slots;
      else if (h->tag == (TAG_SLAB | page->class_index) &&
               h->requested <= CLASS_SIZE[page->class_index])
        ++used_slots;
      else {
        valid = false;
        break;
      }
    }
    if (free_slots != page->free_count || used_slots != page->used_count) {
      valid = false;
      break;
    }
    size_t free_chain = 0;
    for (void *user = page->free_head; user;
         user = *reinterpret_cast<void **>(user)) {
      const uintptr_t base = (uintptr_t)page + sizeof(SlabPage);
      const uintptr_t addr = (uintptr_t)user - sizeof(AllocHeader);
      if (addr < base || (addr - base) % page->stride ||
          (addr - base) / page->stride >= page->capacity) {
        valid = false;
        break;
      }
      AllocHeader *h = reinterpret_cast<AllocHeader *>(addr);
      if (h->tag != TAG_FREE || ++free_chain > page->capacity) {
        valid = false;
        break;
      }
    }
    if (free_chain != page->free_count ||
        (page->on_free_list != (page->free_count != 0))) {
      valid = false;
      break;
    }
    if (page->on_free_list)
      ++expected_free_pages[page->class_index];
    prev_page = page;
  }
  if (page_limit == 0)
    valid = false;
  for (unsigned idx = 0; valid && idx < CLASS_COUNT; ++idx) {
    SlabPage *prev = nullptr;
    size_t list_limit = total_heap_size / SLAB_SIZE + 1;
    size_t listed_pages = 0;
    for (SlabPage *page = static_cast<SlabPage *>(slab_free[idx]);
         page && list_limit--; page = page->free_next) {
      if (page->class_index != idx || !page->free_count ||
          !page->on_free_list || page->free_prev != prev) {
        valid = false;
        break;
      }
      prev = page;
      ++listed_pages;
    }
    if (!list_limit || listed_pages != expected_free_pages[idx])
      valid = false;
  }
  heap_lock.unlock_irqrestore(flags);
  return valid;
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
    size_t used = 0;
    Block *block = static_cast<Block *>(first_block);
    while (block && (uintptr_t)block < (uintptr_t)heap_end) {
      if (block_used(block))
        used += block_size(block);
      block = reinterpret_cast<Block *>((uintptr_t)block + block_size(block));
    }
    info.heap_used_size = used;
  }
  info.heap_free_size = info.heap_total_size - info.heap_used_size;
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
void operator delete(void *ptr, size_t) noexcept { free(ptr); }
void operator delete[](void *ptr, size_t) noexcept { free(ptr); }