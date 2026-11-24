#include <algorithm>
#include <cstdint>
#include <kernel/memory/heap.h>
#include <kernel/memory/pmm.h>
#include <kernel/scheduler.h>
#include <kernel/spinlock.h>
#include <stdlib.h>
#include <string.h>

// Design
// ------
//  * Small objects (<= 512 B): slab pages taken straight from the PMM, one 4
//  KiB
//    page each. No per-object header: ownership lives in a 4-byte-per-slot
//    side table inside the page, "allocated" in a bitmap. 16 B objects cost
//    ~20 B instead of 32 B. Slots are handed out by a bump index first, so
//    fresh pages are never walked or memset.
//  * Medium objects (<= 128 KiB): TLSF (O(1), 8 second-level bins) over PMM
//    backed segments. Used blocks have a 16 B header (size|flags, owner); the
//    boundary tag of a free block lives in its last 8 bytes and is only read
//    when the PREV_FREE flag is set.
//  * Large objects (>= 128 KiB): contiguous PMM page runs, rounded to 4 KiB.
//
// Slab pages never come out of the TLSF arena, so the arena is not punched
// full of 4 KiB-aligned holes. Slabs keep partially used pages in a per-class
// list and hold at most one fully empty page per class in reserve.

namespace Kernel::Memory {

KernelHeap g_kheap;
static Spinlock heap_lock;

namespace {

struct LockGuard {
  uint64_t flags;
  LockGuard() : flags(heap_lock.lock_irqsave()) {}
  ~LockGuard() { heap_lock.unlock_irqrestore(flags); }
  LockGuard(const LockGuard &) = delete;
  LockGuard &operator=(const LockGuard &) = delete;
};

constexpr size_t PAGE = 4096;
constexpr size_t ALIGNMENT = 16;
constexpr size_t HDR = 16;       // used-block header
constexpr size_t MIN_BLOCK = 32; // header + free-list links + footer

constexpr size_t BLOCK_USED = 1;
constexpr size_t BLOCK_PREV_FREE = 2;
constexpr size_t BLOCK_SENTINEL = 4;
constexpr size_t BLOCK_FIRST = 8; // first block of a segment
constexpr size_t BLOCK_FLAGS = 0xF;

constexpr unsigned FL_COUNT = 64;
constexpr unsigned SL_SHIFT = 3;
constexpr unsigned SL_COUNT = 1U << SL_SHIFT;

constexpr size_t SLAB_MAX = 512;
constexpr size_t HUGE_THRESHOLD = 128 * 1024;
constexpr size_t INITIAL_EXPAND_PAGES = 64; // 256 KiB
constexpr size_t MAX_EXPAND_PAGES = 1024;   // 4 MiB per growth step

constexpr uint64_t HUGE_MAGIC = 0x4855474548554745ULL;
constexpr uintptr_t SLAB_COOKIE = 0x534C41425041474EULL;

// ---------------------------------------------------------------- slab ----

constexpr unsigned CLASS_COUNT = 16;
constexpr size_t SLAB_FIXED = 88; // sizeof(SlabPage); owners[] follow

struct ClassInfo {
  uint16_t size;
  uint16_t cap;  // objects per page
  uint16_t base; // offset of slot 0
  uint16_t pad;
  uint32_t recip; // ceil(2^32 / size): exact offset/size for offsets < 4096
};

struct ClassTables {
  ClassInfo info[CLASS_COUNT];
  uint8_t lookup[SLAB_MAX / 16 + 1];
  constexpr ClassTables() : info{}, lookup{} {
    constexpr uint16_t sizes[CLASS_COUNT] = {16,  32,  48,  64,  80,  96,
                                             112, 128, 160, 192, 224, 256,
                                             320, 384, 448, 512};
    for (unsigned c = 0; c < CLASS_COUNT; ++c) {
      const size_t sz = sizes[c];
      size_t n = 256; // used-bitmap holds 256 slots
      size_t base = 0;
      for (; n > 1; --n) {
        base = (SLAB_FIXED + 4 * n + 15) & ~static_cast<size_t>(15);
        if (base + n * sz <= PAGE)
          break;
      }
      info[c].size = static_cast<uint16_t>(sz);
      info[c].cap = static_cast<uint16_t>(n);
      info[c].base = static_cast<uint16_t>(base);
      info[c].pad = 0;
      info[c].recip = static_cast<uint32_t>(((1ull << 32) + sz - 1) / sz);
    }
    for (size_t i = 0; i <= SLAB_MAX / 16; ++i) {
      unsigned c = 0;
      while (c < CLASS_COUNT && info[c].size < i * 16)
        ++c;
      lookup[i] = static_cast<uint8_t>(c);
    }
  }
};

constexpr ClassTables TABLES{};

struct SlabPage {
  uintptr_t cookie; // (address ^ SLAB_COOKIE); zero when released
  SlabPage *all_next;
  SlabPage *all_prev;
  SlabPage *free_next; // partial list (0 < used < capacity)
  SlabPage *free_prev;
  void *free_head; // freed slots (intrusive); fresh slots come from `bump`
  uint16_t capacity;
  uint16_t used;
  uint16_t bump; // slots [0, bump) have been handed out at least once
  uint8_t cls;
  uint8_t on_list;
  uint64_t used_bits[4];
};
static_assert(sizeof(SlabPage) == SLAB_FIXED);

SlabPage *g_slab_free[CLASS_COUNT];  // partial pages
SlabPage *g_slab_empty[CLASS_COUNT]; // at most one empty page per class
size_t g_slab_pages = 0;
size_t g_slab_used = 0; // sum of class sizes of live slab objects

inline uint32_t *slab_owners(SlabPage *p) {
  return reinterpret_cast<uint32_t *>(p + 1);
}
inline uintptr_t slab_base(const SlabPage *p) {
  return reinterpret_cast<uintptr_t>(p) + TABLES.info[p->cls].base;
}
inline unsigned slab_index_of(const SlabPage *p, const ClassInfo &ci,
                              uintptr_t addr) {
  return static_cast<unsigned>(
      (static_cast<uint64_t>(addr - slab_base(p)) * ci.recip) >> 32);
}

// Validates that `ptr` is a live-looking slab slot; no false positives for
// large-block pointers because the cookie is address-dependent.
SlabPage *slab_lookup(void *ptr, unsigned *idx_out) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(ptr);
  auto *page = reinterpret_cast<SlabPage *>(a & ~(PAGE - 1));
  if (page->cookie != (reinterpret_cast<uintptr_t>(page) ^ SLAB_COOKIE))
    return nullptr;
  if (__builtin_expect(page->cls >= CLASS_COUNT, 0))
    return nullptr;
  const ClassInfo &ci = TABLES.info[page->cls];
  const uintptr_t base = slab_base(page);
  if (a < base)
    return nullptr;
  const size_t off = a - base;
  if (off >= static_cast<size_t>(ci.cap) * ci.size)
    return nullptr;
  const unsigned idx =
      static_cast<unsigned>((static_cast<uint64_t>(off) * ci.recip) >> 32);
  if (idx * ci.size != off)
    return nullptr;
  if (idx_out)
    *idx_out = idx;
  return page;
}

// -------------------------------------------------------------- blocks ----

struct Block {
  size_t size_flags;
  union {
    uint64_t owner;   // used
    Block *free_prev; // free
  };
  Block *free_next; // free only
};
static_assert(offsetof(Block, free_next) == 16);

struct HugeAllocHeader {
  uint64_t magic;
  size_t size;           // bytes of the whole page run
  HugeAllocHeader *next; // intrusive list so thread exit can reclaim them
  HugeAllocHeader *prev;
  uint64_t owner;
  uint64_t pad;
};
static_assert(sizeof(HugeAllocHeader) == 48);

HugeAllocHeader *g_huge_head = nullptr;

// Caller holds heap_lock.
void huge_unlink(HugeAllocHeader *h) {
  if (h->prev)
    h->prev->next = h->next;
  else
    g_huge_head = h->next;
  if (h->next)
    h->next->prev = h->prev;
  h->magic = 0;
}

struct alignas(16) HeapSegment {
  HeapSegment *next;
  HeapSegment *prev;
  void *raw_pages;
  size_t page_count;
  uintptr_t begin;
  uintptr_t end;
};
static_assert(sizeof(HeapSegment) == 48);

HeapSegment *g_segments_head = nullptr;
size_t g_next_expand_pages = INITIAL_EXPAND_PAGES;

inline size_t bsize(const Block *b) { return b->size_flags & ~BLOCK_FLAGS; }
inline bool bused(const Block *b) { return b->size_flags & BLOCK_USED; }
inline Block *bat(const Block *b, size_t off) {
  return reinterpret_cast<Block *>(reinterpret_cast<uintptr_t>(b) + off);
}
inline size_t &footer(Block *b, size_t size) {
  return *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(b) + size - 8);
}

inline size_t align_up_size(size_t n, size_t a) {
  return (n + a - 1) & ~(a - 1);
}
inline unsigned msb_index(size_t n) {
  return 63u - static_cast<unsigned>(__builtin_clzll(n));
}
inline size_t block_need(size_t payload) {
  return std::max(MIN_BLOCK, align_up_size(payload + HDR, ALIGNMENT));
}

// TLSF mapping in 16-byte units. u < 8: exact bin (fl 0). Else 8 bins per
// power of two.
inline void map_insert(size_t size, unsigned &fl, unsigned &sl) {
  const size_t u = size >> 4;
  if (u < SL_COUNT) {
    fl = 0;
    sl = static_cast<unsigned>(u);
    return;
  }
  const unsigned f = msb_index(u);
  fl = f - (SL_SHIFT - 1);
  sl = static_cast<unsigned>(u >> (f - SL_SHIFT)) - SL_COUNT;
}

// Round up so every block in the found bin is large enough (good-fit).
inline void map_search(size_t size, unsigned &fl, unsigned &sl) {
  const size_t u = size >> 4;
  if (u >= SL_COUNT) {
    const size_t step = static_cast<size_t>(1) << (msb_index(u) - SL_SHIFT);
    size = ((u + step - 1) & ~(step - 1)) << 4;
  }
  map_insert(size, fl, sl);
}

HugeAllocHeader *huge_header(void *ptr) {
  if ((reinterpret_cast<uintptr_t>(ptr) & (PAGE - 1)) !=
      sizeof(HugeAllocHeader))
    return nullptr;
  auto *h = reinterpret_cast<HugeAllocHeader *>(
      reinterpret_cast<uintptr_t>(ptr) - sizeof(HugeAllocHeader));
  if (h->magic != HUGE_MAGIC || h->size < PAGE || (h->size & (PAGE - 1)))
    return nullptr;
  return h;
}

// Free block of `size` at `b`; caller guarantees its predecessor is used.
inline void make_free(Block *b, size_t size, size_t keep_flags) {
  b->size_flags = size | keep_flags;
  footer(b, size) = size;
  bat(b, size)->size_flags |= BLOCK_PREV_FREE;
}

void register_segment(uintptr_t seg_begin, uintptr_t seg_end, void *raw_pages,
                      size_t page_count, void *&heap_begin, void *&heap_end,
                      void *&first_block, size_t &total_heap_size,
                      Block *&out_first) {
  auto *seg = reinterpret_cast<HeapSegment *>(seg_begin);
  const uintptr_t first_addr = seg_begin + sizeof(HeapSegment);
  const uintptr_t sentinel_addr = seg_end - HDR;
  const size_t usable = sentinel_addr - first_addr;

  seg->raw_pages = raw_pages;
  seg->page_count = page_count;
  seg->begin = first_addr;
  seg->end = seg_end;
  seg->prev = nullptr;
  seg->next = g_segments_head;
  if (g_segments_head)
    g_segments_head->prev = seg;
  g_segments_head = seg;

  if (!heap_begin || first_addr < reinterpret_cast<uintptr_t>(heap_begin))
    heap_begin = reinterpret_cast<void *>(first_addr);
  if (!heap_end || seg_end > reinterpret_cast<uintptr_t>(heap_end))
    heap_end = reinterpret_cast<void *>(seg_end);
  if (!first_block)
    first_block = reinterpret_cast<void *>(first_addr);

  auto *sentinel = reinterpret_cast<Block *>(sentinel_addr);
  sentinel->size_flags = HDR | BLOCK_USED | BLOCK_SENTINEL;

  auto *first = reinterpret_cast<Block *>(first_addr);
  make_free(first, usable, BLOCK_FIRST); // also sets sentinel PREV_FREE
  first->free_prev = first->free_next = nullptr;

  total_heap_size += usable;
  out_first = first;
}

bool expand_heap(size_t min_block, void *&heap_begin, void *&heap_end,
                 void *&first_block, size_t &total_heap_size,
                 Block *&out_first) {
  constexpr size_t kSegMeta = sizeof(HeapSegment) + HDR;
  if (__builtin_expect(min_block > static_cast<size_t>(-1) / 2, 0))
    return false;

  const size_t min_pages = (min_block + kSegMeta + PAGE - 1) / PAGE;
  size_t pages = std::max(g_next_expand_pages, min_pages);

  void *raw = nullptr;
  for (;;) {
    raw = g_pmm.allocate_page_block(pages);
    if (raw || pages == min_pages)
      break;
    pages = std::max(min_pages, pages >> 1);
  }
  if (__builtin_expect(!raw, 0))
    return false;

  if (pages >= g_next_expand_pages && g_next_expand_pages < MAX_EXPAND_PAGES)
    g_next_expand_pages = std::min(MAX_EXPAND_PAGES, pages << 1);

  const uintptr_t base = reinterpret_cast<uintptr_t>(raw);
  register_segment(base, base + pages * PAGE, raw, pages, heap_begin, heap_end,
                   first_block, total_heap_size, out_first);
  return true;
}

void destroy_segment(HeapSegment *seg, void *&heap_begin, void *&heap_end,
                     void *&first_block, size_t &total_heap_size) {
  if (seg->prev)
    seg->prev->next = seg->next;
  else
    g_segments_head = seg->next;
  if (seg->next)
    seg->next->prev = seg->prev;

  total_heap_size -= (seg->end - HDR) - seg->begin;

  void *raw = seg->raw_pages;
  const size_t pages = seg->page_count;

  uintptr_t min_b = static_cast<uintptr_t>(-1), max_e = 0;
  for (HeapSegment *s = g_segments_head; s; s = s->next) {
    min_b = std::min(min_b, s->begin);
    max_e = std::max(max_e, s->end);
  }
  heap_begin = g_segments_head ? reinterpret_cast<void *>(min_b) : nullptr;
  heap_end = g_segments_head ? reinterpret_cast<void *>(max_e) : nullptr;
  first_block = g_segments_head
                    ? reinterpret_cast<void *>(g_segments_head->begin)
                    : nullptr;

  g_pmm.free_page_block(raw, pages);
}

} // namespace

// ------------------------------------------------------------ TLSF core ----

void KernelHeap::insert_free_block(void *raw) {
  auto *b = static_cast<Block *>(raw);
  unsigned fl, sl;
  map_insert(bsize(b), fl, sl);
  auto *head = static_cast<Block *>(free_bins[fl][sl]);
  b->free_prev = nullptr;
  b->free_next = head;
  if (head)
    head->free_prev = b;
  free_bins[fl][sl] = b;
  sl_bitmap[fl] |= static_cast<uint8_t>(1u << sl);
  fl_bitmap |= static_cast<uint64_t>(1) << fl;
}

void KernelHeap::remove_free_block(void *raw) {
  auto *b = static_cast<Block *>(raw);
  unsigned fl, sl;
  map_insert(bsize(b), fl, sl);
  if (b->free_prev)
    b->free_prev->free_next = b->free_next;
  else
    free_bins[fl][sl] = b->free_next;
  if (b->free_next)
    b->free_next->free_prev = b->free_prev;
  if (!free_bins[fl][sl]) {
    sl_bitmap[fl] &= static_cast<uint8_t>(~(1u << sl));
    if (!sl_bitmap[fl])
      fl_bitmap &= ~(static_cast<uint64_t>(1) << fl);
  }
}

void *KernelHeap::find_suitable_block(size_t size) {
  unsigned fl, sl;
  map_search(size, fl, sl);
  const unsigned avail = sl_bitmap[fl] & (0xFFu << sl);
  if (avail)
    return free_bins[fl][__builtin_ctz(avail)];
  if (fl + 1 >= FL_COUNT)
    return nullptr;
  const uint64_t higher = fl_bitmap & (~static_cast<uint64_t>(0) << (fl + 1));
  if (!higher)
    return nullptr;
  fl = static_cast<unsigned>(__builtin_ctzll(higher));
  return free_bins[fl][__builtin_ctz(static_cast<unsigned>(sl_bitmap[fl]))];
}

// Only ALIGNMENT-aligned payloads are supported (slabs no longer live here).
void *KernelHeap::allocate_block(size_t payload_size, size_t alignment,
                                 uint32_t flags, void **block_out) {
  (void)flags;
  if (__builtin_expect(!first_block || alignment > ALIGNMENT ||
                           payload_size > static_cast<size_t>(-1) / 2,
                       0))
    return nullptr;

  const size_t need = block_need(payload_size);
  auto *b = static_cast<Block *>(find_suitable_block(need));
  if (__builtin_expect(!b, 0)) {
    Block *fresh = nullptr;
    if (!expand_heap(need, heap_begin, heap_end, first_block, total_heap_size,
                     fresh))
      return nullptr;
    insert_free_block(fresh);
    b = static_cast<Block *>(find_suitable_block(need));
    if (__builtin_expect(!b, 0))
      return nullptr;
  }
  remove_free_block(b);

  const size_t bsz = bsize(b);
  const size_t keep = b->size_flags & BLOCK_FIRST; // predecessor is used
  size_t total = bsz;
  const size_t rem = bsz - need;
  if (rem >= MIN_BLOCK) {
    total = need;
    Block *tail = bat(b, need);
    tail->size_flags = rem; // successor already has PREV_FREE set
    footer(tail, rem) = rem;
    insert_free_block(tail);
  } else {
    bat(b, bsz)->size_flags &= ~BLOCK_PREV_FREE;
  }
  b->size_flags = total | BLOCK_USED | keep;

  used_bytes_count += total;
  const size_t cur = used_bytes_count + g_slab_used;
  if (cur > peak_used_size)
    peak_used_size = cur;
  if (block_out)
    *block_out = b;
  return reinterpret_cast<char *>(b) + HDR;
}

// Frees and coalesces. Returns the merged free block, or nullptr when a whole
// segment was handed back to the PMM.
void *KernelHeap::release_block(void *raw) {
  auto *b = static_cast<Block *>(raw);
  size_t size = bsize(b);
  used_bytes_count -= size;

  size_t keep = b->size_flags & BLOCK_FIRST;

  Block *next = bat(b, size);
  if (!bused(next)) {
    remove_free_block(next);
    size += bsize(next);
  }
  if (b->size_flags & BLOCK_PREV_FREE) {
    const size_t psz =
        *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(b) - 8);
    Block *prev = bat(b, 0 - psz);
    remove_free_block(prev);
    size += psz;
    keep = prev->size_flags & BLOCK_FIRST;
    b = prev;
  }

  Block *after = bat(b, size);
  if ((keep & BLOCK_FIRST) && (after->size_flags & BLOCK_SENTINEL)) {
    auto *seg = reinterpret_cast<HeapSegment *>(reinterpret_cast<uintptr_t>(b) -
                                                sizeof(HeapSegment));
    if (seg->raw_pages && (seg->prev || seg->next)) {
      destroy_segment(seg, heap_begin, heap_end, first_block, total_heap_size);
      return nullptr;
    }
  }

  make_free(b, size, keep);
  insert_free_block(b);
  return b;
}

bool KernelHeap::resize_large_locked(void *raw_block, size_t size) {
  auto *b = static_cast<Block *>(raw_block);
  if (__builtin_expect(!size || size >= HUGE_THRESHOLD, 0))
    return false;

  const size_t needed = block_need(size);
  const size_t old = bsize(b);
  // A used block may legitimately have a free predecessor: keep PREV_FREE.
  const size_t flags =
      b->size_flags & (BLOCK_USED | BLOCK_FIRST | BLOCK_PREV_FREE);

  if (needed <= old) {
    const size_t rem = old - needed;
    if (rem >= MIN_BLOCK) {
      b->size_flags = needed | flags;
      Block *tail = bat(b, needed);
      size_t tsz = rem;
      Block *nx = bat(tail, rem);
      if (!bused(nx)) {
        remove_free_block(nx);
        tsz += bsize(nx);
      }
      make_free(tail, tsz, 0);
      insert_free_block(tail);
      used_bytes_count -= rem;
    }
    return true;
  }

  Block *nx = bat(b, old);
  if (bused(nx))
    return false;
  const size_t nsz = bsize(nx);
  if (old + nsz < needed)
    return false;
  remove_free_block(nx);

  const size_t combined = old + nsz;
  const size_t rem = combined - needed;
  if (rem >= MIN_BLOCK) {
    b->size_flags = needed | flags;
    Block *tail = bat(b, needed);
    make_free(tail, rem, 0); // successor already PREV_FREE
    insert_free_block(tail);
    used_bytes_count += needed - old;
  } else {
    b->size_flags = combined | flags;
    bat(b, combined)->size_flags &= ~BLOCK_PREV_FREE;
    used_bytes_count += nsz;
  }
  const size_t cur = used_bytes_count + g_slab_used;
  if (cur > peak_used_size)
    peak_used_size = cur;
  return true;
}

// ---------------------------------------------------------------- slabs ----

void KernelHeap::add_slab_free_page(void *raw) {
  auto *page = static_cast<SlabPage *>(raw);
  if (page->on_list)
    return;
  SlabPage *head = g_slab_free[page->cls];
  page->free_prev = nullptr;
  page->free_next = head;
  if (head)
    head->free_prev = page;
  g_slab_free[page->cls] = page;
  page->on_list = 1;
}

void KernelHeap::remove_slab_free_page(void *raw) {
  auto *page = static_cast<SlabPage *>(raw);
  if (!page->on_list)
    return;
  if (page->free_prev)
    page->free_prev->free_next = page->free_next;
  else
    g_slab_free[page->cls] = page->free_next;
  if (page->free_next)
    page->free_next->free_prev = page->free_prev;
  page->free_prev = page->free_next = nullptr;
  page->on_list = 0;
}

void KernelHeap::unlink_slab_page(void *raw) {
  auto *page = static_cast<SlabPage *>(raw);
  if (page->all_prev)
    page->all_prev->all_next = page->all_next;
  else
    slab_pages = page->all_next;
  if (page->all_next)
    page->all_next->all_prev = page->all_prev;
  page->all_prev = page->all_next = nullptr;
}

void KernelHeap::release_slab_page(void *raw) {
  auto *page = static_cast<SlabPage *>(raw);
  if (g_slab_empty[page->cls] == page)
    g_slab_empty[page->cls] = nullptr;
  remove_slab_free_page(page);
  unlink_slab_page(page);
  page->cookie = 0;
  --g_slab_pages;
  g_pmm.free_page_block(page, 1);
}

void *KernelHeap::allocate_slab_locked(unsigned class_index, size_t,
                                       uint64_t owner) {
  const ClassInfo &ci = TABLES.info[class_index];

  SlabPage *page = g_slab_free[class_index];
  if (__builtin_expect(!page, 0)) {
    page = g_slab_empty[class_index];
    if (page) {
      g_slab_empty[class_index] = nullptr;
    } else {
      void *mem = g_pmm.allocate_page_block(1);
      if (__builtin_expect(!mem, 0))
        return nullptr;
      page = static_cast<SlabPage *>(mem);
      page->cookie = reinterpret_cast<uintptr_t>(page) ^ SLAB_COOKIE;
      page->free_head = nullptr;
      page->capacity = ci.cap;
      page->used = 0;
      page->bump = 0;
      page->cls = static_cast<uint8_t>(class_index);
      page->on_list = 0;
      page->used_bits[0] = page->used_bits[1] = page->used_bits[2] =
          page->used_bits[3] = 0;
      page->free_prev = page->free_next = nullptr;
      page->all_prev = nullptr;
      page->all_next = static_cast<SlabPage *>(slab_pages);
      if (page->all_next)
        page->all_next->all_prev = page;
      slab_pages = page;
      ++g_slab_pages;
    }
    add_slab_free_page(page);
  }

  void *user;
  unsigned idx;
  if (page->free_head) {
    user = page->free_head;
    page->free_head = *static_cast<void **>(user);
    idx = slab_index_of(page, ci, reinterpret_cast<uintptr_t>(user));
  } else {
    idx = page->bump++;
    user = reinterpret_cast<void *>(slab_base(page) +
                                    static_cast<size_t>(idx) * ci.size);
  }

  page->used_bits[idx >> 6] |= 1ull << (idx & 63);
  slab_owners(page)[idx] = static_cast<uint32_t>(owner);
  ++page->used;
  if (page->used == page->capacity)
    remove_slab_free_page(page);

  g_slab_used += ci.size;
  const size_t cur = used_bytes_count + g_slab_used;
  if (cur > peak_used_size)
    peak_used_size = cur;
  return user;
}

void KernelHeap::free_slab_locked(void *ptr, void *, void *raw_page) {
  auto *page = static_cast<SlabPage *>(raw_page);
  const ClassInfo &ci = TABLES.info[page->cls];
  const unsigned idx =
      slab_index_of(page, ci, reinterpret_cast<uintptr_t>(ptr));
  const uint64_t bit = 1ull << (idx & 63);
  uint64_t &word = page->used_bits[idx >> 6];
  if (__builtin_expect(!(word & bit), 0))
    return; // double free
  word &= ~bit;

  g_slab_used -= ci.size;
  const bool was_full = page->used == page->capacity;
  --page->used;

  if (page->used == 0) {
    // Reset to pristine so the page is served by the bump index again.
    page->free_head = nullptr;
    page->bump = 0;
    remove_slab_free_page(page);
    SlabPage *&cached = g_slab_empty[page->cls];
    if (cached && cached != page)
      release_slab_page(page);
    else
      cached = page;
    return;
  }

  *static_cast<void **>(ptr) = page->free_head;
  page->free_head = ptr;
  if (was_full)
    add_slab_free_page(page);
}

// ----------------------------------------------------------- allocation ----

void *KernelHeap::allocate_locked(size_t size, uint64_t owner) {
  if (__builtin_expect(!size, 0))
    return nullptr;
  if (size <= SLAB_MAX)
    return allocate_slab_locked(TABLES.lookup[(size + 15) >> 4], size, owner);

  void *block = nullptr;
  void *payload = allocate_block(size, ALIGNMENT, 0, &block);
  if (__builtin_expect(!payload, 0))
    return nullptr;
  static_cast<Block *>(block)->owner = owner;
  return payload;
}

void KernelHeap::free_locked(void *ptr) {
  if (__builtin_expect(!ptr, 0))
    return;
  if (SlabPage *page = slab_lookup(ptr, nullptr)) {
    free_slab_locked(ptr, nullptr, page);
    return;
  }
  if (reinterpret_cast<uintptr_t>(ptr) & (ALIGNMENT - 1))
    return;
  auto *b = reinterpret_cast<Block *>(reinterpret_cast<uintptr_t>(ptr) - HDR);
  if ((b->size_flags & (BLOCK_USED | BLOCK_SENTINEL)) != BLOCK_USED)
    return; // not a live block (double free / bad pointer)
  release_block(b);
}

void KernelHeap::init(void *heap_start_addr, size_t initial_size) {
  LockGuard guard;
  heap_begin = heap_end = first_block = slab_pages = nullptr;
  g_segments_head = nullptr;
  g_next_expand_pages = INITIAL_EXPAND_PAGES;
  fl_bitmap = 0;
  used_bytes_count = peak_used_size = total_heap_size = 0;
  g_slab_pages = g_slab_used = 0;
  g_huge_head = nullptr;
  for (unsigned fl = 0; fl < FL_COUNT; ++fl) {
    sl_bitmap[fl] = 0;
    for (unsigned sl = 0; sl < SL_COUNT; ++sl)
      free_bins[fl][sl] = nullptr;
  }
  for (unsigned i = 0; i < CLASS_COUNT; ++i)
    g_slab_free[i] = g_slab_empty[i] = nullptr;

  const uintptr_t raw = reinterpret_cast<uintptr_t>(heap_start_addr);
  constexpr size_t kSegMeta = sizeof(HeapSegment) + HDR;
  if (heap_start_addr && initial_size &&
      raw <= static_cast<uintptr_t>(-1) - (ALIGNMENT - 1) &&
      initial_size <= static_cast<uintptr_t>(-1) - raw) {
    const uintptr_t begin = (raw + ALIGNMENT - 1) & ~(uintptr_t)(ALIGNMENT - 1);
    const uintptr_t end = (raw + initial_size) & ~(uintptr_t)(ALIGNMENT - 1);
    if (end > begin && end - begin >= kSegMeta + MIN_BLOCK) {
      Block *first = nullptr;
      register_segment(begin, end, nullptr, 0, heap_begin, heap_end,
                       first_block, total_heap_size, first);
      insert_free_block(first);
    }
  }
}

void *KernelHeap::kmalloc_huge(size_t size) {
  if (__builtin_expect(size > static_cast<size_t>(-1) -
                                  sizeof(HugeAllocHeader) - (PAGE - 1),
                       0))
    return nullptr;
  const size_t pages = (size + sizeof(HugeAllocHeader) + PAGE - 1) / PAGE;

  void *base = g_pmm.allocate_page_block(pages); // contiguous run
  if (__builtin_expect(!base, 0))
    return nullptr;

  auto *h = static_cast<HugeAllocHeader *>(base);
  h->magic = HUGE_MAGIC;
  h->size = pages * PAGE;
  h->owner = Kernel::Scheduler::current_thread_id();
  h->pad = 0;
  {
    LockGuard guard;
    h->prev = nullptr;
    h->next = g_huge_head;
    if (g_huge_head)
      g_huge_head->prev = h;
    g_huge_head = h;
  }
  return h + 1;
}

void *KernelHeap::kmalloc(size_t size) {
  if (__builtin_expect(!size, 0))
    return nullptr;
  if (__builtin_expect(size >= HUGE_THRESHOLD, 0))
    return kmalloc_huge(size);
  // Resolve the owner before taking the heap lock: no heap -> scheduler lock
  // nesting and a shorter IRQ-off window.
  const uint64_t owner = Kernel::Scheduler::current_thread_id();
  LockGuard guard;
  return allocate_locked(size, owner);
}

void KernelHeap::kfree(void *ptr) {
  if (__builtin_expect(!ptr, 0))
    return;
  if (__builtin_expect(huge_header(ptr) != nullptr, 0)) {
    HugeAllocHeader *h = huge_header(ptr);
    const size_t pages = h->size / PAGE;
    {
      LockGuard guard;
      huge_unlink(h);
    }
    g_pmm.free_page_block(h, pages);
    return;
  }
  LockGuard guard;
  free_locked(ptr);
}

void KernelHeap::free_thread_allocations(uint64_t thread_id) {
  if (!thread_id)
    return;
  const uint32_t tid32 = static_cast<uint32_t>(thread_id);
  LockGuard guard;

  // Medium blocks: every used block in the arena is a medium allocation.
  for (HeapSegment *seg = g_segments_head; seg;) {
    HeapSegment *next_seg = seg->next;
    const uintptr_t limit = seg->end - HDR;
    uintptr_t cur = seg->begin;
    while (cur < limit) {
      auto *b = reinterpret_cast<Block *>(cur);
      if (bused(b) && b->owner == thread_id) {
        auto *merged = static_cast<Block *>(release_block(b));
        if (!merged)
          break; // segment returned to the PMM
        cur = reinterpret_cast<uintptr_t>(merged) + bsize(merged);
        continue;
      }
      cur += bsize(b);
    }
    seg = next_seg;
  }

  // Huge allocations.
  for (HugeAllocHeader *h = g_huge_head; h;) {
    HugeAllocHeader *next = h->next;
    if (h->owner == thread_id) {
      const size_t pages = h->size / PAGE;
      huge_unlink(h);
      g_pmm.free_page_block(h, pages);
    }
    h = next;
  }

  // Slab objects.
  for (SlabPage *page = static_cast<SlabPage *>(slab_pages); page;) {
    SlabPage *next = page->all_next;
    if (page->used) {
      const ClassInfo &ci = TABLES.info[page->cls];
      const uint32_t *owners = slab_owners(page);
      const uintptr_t base = slab_base(page);
      bool gone = false;
      for (unsigned w = 0; w < 4 && !gone; ++w) {
        uint64_t bits = page->used_bits[w];
        while (bits) {
          const unsigned idx = w * 64 + __builtin_ctzll(bits);
          bits &= bits - 1;
          if (owners[idx] != tid32)
            continue;
          const bool last = page->used == 1; // page may vanish below
          free_slab_locked(reinterpret_cast<void *>(
                               base + static_cast<size_t>(idx) * ci.size),
                           nullptr, page);
          if (last) {
            gone = true;
            break;
          }
        }
      }
    }
    page = next;
  }
}

// Reports usable bytes (slot / block capacity), not the exact requested size.
size_t KernelHeap::get_thread_allocated_size(uint64_t thread_id) const {
  if (!thread_id)
    return 0;
  const uint32_t tid32 = static_cast<uint32_t>(thread_id);
  LockGuard guard;
  size_t total = 0;
  for (HeapSegment *seg = g_segments_head; seg; seg = seg->next) {
    const uintptr_t limit = seg->end - HDR;
    for (uintptr_t cur = seg->begin; cur < limit;) {
      auto *b = reinterpret_cast<Block *>(cur);
      if (bused(b) && b->owner == thread_id)
        total += bsize(b) - HDR;
      cur += bsize(b);
    }
  }
  for (HugeAllocHeader *h = g_huge_head; h; h = h->next)
    if (h->owner == thread_id)
      total += h->size - sizeof(HugeAllocHeader);
  for (SlabPage *page = static_cast<SlabPage *>(slab_pages); page;
       page = page->all_next) {
    const uint32_t *owners = slab_owners(page);
    const size_t sz = TABLES.info[page->cls].size;
    for (unsigned w = 0; w < 4; ++w) {
      uint64_t bits = page->used_bits[w];
      while (bits) {
        const unsigned idx = w * 64 + __builtin_ctzll(bits);
        bits &= bits - 1;
        if (owners[idx] == tid32)
          total += sz;
      }
    }
  }
  return total;
}

void *KernelHeap::krealloc(void *ptr, size_t size) {
  if (!ptr)
    return kmalloc(size);
  if (!size) {
    kfree(ptr);
    return nullptr;
  }

  size_t old_usable;
  uint64_t owner;

  if (HugeAllocHeader *h = huge_header(ptr)) {
    old_usable = h->size - sizeof(HugeAllocHeader);
    if (size >= HUGE_THRESHOLD && size <= static_cast<size_t>(-1) -
                                              sizeof(HugeAllocHeader) -
                                              (PAGE - 1)) {
      const size_t new_pages =
          (size + sizeof(HugeAllocHeader) + PAGE - 1) / PAGE;
      const size_t old_pages = h->size / PAGE;
      if (new_pages == old_pages)
        return ptr;
      if (new_pages < old_pages) { // shrink in place, give the tail back
        g_pmm.free_page_block(reinterpret_cast<char *>(h) + new_pages * PAGE,
                              old_pages - new_pages);
        h->size = new_pages * PAGE;
        return ptr;
      }
    }
    owner = h->owner;
    void *np;
    if (size >= HUGE_THRESHOLD) {
      np = kmalloc_huge(size);
      if (np) {
        LockGuard guard;
        huge_header(np)->owner = owner;
      }
    } else {
      LockGuard guard;
      np = allocate_locked(size, owner);
    }
    if (!np)
      return nullptr;
    memcpy(np, ptr, std::min(old_usable, size));
    kfree(ptr);
    return np;
  }

  void *np = nullptr;
  {
    LockGuard guard;
    unsigned idx;
    if (SlabPage *page = slab_lookup(ptr, &idx)) {
      const size_t csz = TABLES.info[page->cls].size;
      if (size <= csz && csz <= 2 * align_up_size(size, ALIGNMENT))
        return ptr; // fits and not wasteful
      old_usable = csz;
      owner = slab_owners(page)[idx];
    } else {
      if (reinterpret_cast<uintptr_t>(ptr) & (ALIGNMENT - 1))
        return nullptr;
      auto *b =
          reinterpret_cast<Block *>(reinterpret_cast<uintptr_t>(ptr) - HDR);
      if ((b->size_flags & (BLOCK_USED | BLOCK_SENTINEL)) != BLOCK_USED)
        return nullptr;
      if (size > SLAB_MAX && size < HUGE_THRESHOLD &&
          resize_large_locked(b, size))
        return ptr;
      old_usable = bsize(b) - HDR;
      owner = b->owner;
    }
    if (size < HUGE_THRESHOLD)
      np = allocate_locked(size, owner);
  }

  if (size >= HUGE_THRESHOLD) {
    np = kmalloc_huge(size);
    if (np) {
      LockGuard guard;
      huge_header(np)->owner = owner;
    }
  }
  if (!np)
    return nullptr; // original block untouched

  memcpy(np, ptr, std::min(old_usable, size)); // outside the IRQ-off section
  kfree(ptr);
  return np;
}

void *KernelHeap::kcalloc(size_t num, size_t size) {
  if (size && num > static_cast<size_t>(-1) / size)
    return nullptr;
  const size_t total = num * size;
  void *ptr = kmalloc(total);
  if (ptr)
    memset(ptr, 0, total);
  return ptr;
}

void *KernelHeap::kzalloc(size_t size) { return kcalloc(1, size); }

// ---------------------------------------------------------------- stats ----

size_t KernelHeap::get_total_size() const {
  LockGuard guard;
  return total_heap_size + g_slab_pages * PAGE;
}

size_t KernelHeap::get_used_size() const {
  LockGuard guard;
  return used_bytes_count + g_slab_used;
}

size_t KernelHeap::get_free_size() const {
  LockGuard guard;
  const size_t total = total_heap_size + g_slab_pages * PAGE;
  const size_t used = used_bytes_count + g_slab_used;
  return used <= total ? total - used : 0;
}

size_t KernelHeap::get_peak_used_size() const {
  LockGuard guard;
  return peak_used_size;
}

bool KernelHeap::validate() const {
  LockGuard guard;

  if (!heap_begin || !heap_end || !first_block || !g_segments_head ||
      reinterpret_cast<uintptr_t>(heap_begin) >=
          reinterpret_cast<uintptr_t>(heap_end))
    return false;

  size_t phys_used = 0, phys_free = 0, total_usable = 0;
  HeapSegment *prev_seg = nullptr;
  for (HeapSegment *seg = g_segments_head; seg; seg = seg->next) {
    if (seg->prev != prev_seg || seg->end < seg->begin + HDR + MIN_BLOCK)
      return false;
    const uintptr_t limit = seg->end - HDR;
    total_usable += limit - seg->begin;
    bool prev_free = false, first = true;
    uintptr_t cur = seg->begin;
    while (cur < limit) {
      auto *b = reinterpret_cast<Block *>(cur);
      const size_t size = bsize(b);
      if ((cur & (ALIGNMENT - 1)) || size < MIN_BLOCK ||
          (size & (ALIGNMENT - 1)) || size > limit - cur ||
          (b->size_flags & BLOCK_SENTINEL) ||
          static_cast<bool>(b->size_flags & BLOCK_FIRST) != first ||
          static_cast<bool>(b->size_flags & BLOCK_PREV_FREE) != prev_free)
        return false;
      if (bused(b)) {
        phys_used += size;
      } else {
        if (prev_free || footer(b, size) != size)
          return false;
        ++phys_free;
      }
      prev_free = !bused(b);
      first = false;
      cur += size;
    }
    auto *sentinel = reinterpret_cast<Block *>(limit);
    if (cur != limit ||
        sentinel->size_flags != (HDR | BLOCK_USED | BLOCK_SENTINEL |
                                 (prev_free ? BLOCK_PREV_FREE : 0)))
      return false;
    prev_seg = seg;
  }
  if (total_usable != total_heap_size || phys_used != used_bytes_count)
    return false;

  uint64_t expected_fl = 0;
  size_t listed = 0;
  for (unsigned fl = 0; fl < FL_COUNT; ++fl) {
    uint8_t expected_sl = 0;
    for (unsigned sl = 0; sl < SL_COUNT; ++sl) {
      Block *prev = nullptr;
      auto *b = static_cast<Block *>(free_bins[fl][sl]);
      size_t guard_n = phys_free + 1;
      while (b && guard_n--) {
        unsigned afl, asl;
        if (bused(b) || b->free_prev != prev)
          return false;
        map_insert(bsize(b), afl, asl);
        if (afl != fl || asl != sl)
          return false;
        prev = b;
        b = b->free_next;
        ++listed;
      }
      if (b)
        return false;
      if (free_bins[fl][sl])
        expected_sl |= static_cast<uint8_t>(1u << sl);
    }
    if (expected_sl)
      expected_fl |= static_cast<uint64_t>(1) << fl;
    if (expected_sl != sl_bitmap[fl])
      return false;
  }
  if (expected_fl != fl_bitmap || listed != phys_free)
    return false;

  // Slabs.
  size_t pages = 0, used_bytes = 0;
  size_t partial[CLASS_COUNT] = {};
  bool empty_seen[CLASS_COUNT] = {};
  SlabPage *prev_page = nullptr;
  for (SlabPage *page = static_cast<SlabPage *>(slab_pages); page;
       page = page->all_next) {
    if (++pages > g_slab_pages || page->all_prev != prev_page ||
        page->cookie != (reinterpret_cast<uintptr_t>(page) ^ SLAB_COOKIE) ||
        page->cls >= CLASS_COUNT)
      return false;
    const ClassInfo &ci = TABLES.info[page->cls];
    if (page->capacity != ci.cap || page->used > page->capacity ||
        page->bump > page->capacity || page->used > page->bump)
      return false;
    unsigned bits = 0;
    for (unsigned w = 0; w < 4; ++w) {
      bits += static_cast<unsigned>(__builtin_popcountll(page->used_bits[w]));
      for (uint64_t m = page->used_bits[w]; m; m &= m - 1)
        if (w * 64 + static_cast<unsigned>(__builtin_ctzll(m)) >= page->bump)
          return false;
    }
    if (bits != page->used)
      return false;
    size_t chain = 0;
    for (void *u = page->free_head; u; u = *static_cast<void **>(u)) {
      const uintptr_t a = reinterpret_cast<uintptr_t>(u);
      if (a < slab_base(page) || ++chain > page->bump)
        return false;
      const size_t off = a - slab_base(page);
      if (off >= static_cast<size_t>(page->bump) * ci.size)
        return false;
      const unsigned idx = slab_index_of(page, ci, a);
      if (idx * ci.size != off || (page->used_bits[idx >> 6] >> (idx & 63) & 1))
        return false;
    }
    if (chain + page->used != page->bump)
      return false;
    const bool want_listed = page->used > 0 && page->used < page->capacity;
    if (static_cast<bool>(page->on_list) != want_listed)
      return false;
    if (page->used == 0) {
      if (g_slab_empty[page->cls] != page || page->bump != 0)
        return false;
      empty_seen[page->cls] = true;
    }
    if (want_listed)
      ++partial[page->cls];
    used_bytes += static_cast<size_t>(page->used) * ci.size;
    prev_page = page;
  }
  if (pages != g_slab_pages || used_bytes != g_slab_used)
    return false;
  for (unsigned c = 0; c < CLASS_COUNT; ++c) {
    if (g_slab_empty[c] && !empty_seen[c])
      return false;
    size_t n = 0;
    SlabPage *prev = nullptr;
    for (SlabPage *p = g_slab_free[c]; p; p = p->free_next) {
      if (++n > partial[c] || p->cls != c || !p->on_list ||
          p->free_prev != prev)
        return false;
      prev = p;
    }
    if (n != partial[c])
      return false;
  }
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

  LockGuard guard;
  size_t used = used_bytes_count + g_slab_used;
  if (recheck) {
    used = 0;
    for (HeapSegment *seg = g_segments_head; seg; seg = seg->next) {
      const uintptr_t limit = seg->end - HDR;
      for (uintptr_t cur = seg->begin; cur < limit;) {
        auto *b = reinterpret_cast<Block *>(cur);
        if (bused(b))
          used += bsize(b);
        cur += bsize(b);
      }
    }
    for (SlabPage *p = static_cast<SlabPage *>(slab_pages); p; p = p->all_next)
      used += static_cast<size_t>(p->used) * TABLES.info[p->cls].size;
  }
  info.heap_total_size = total_heap_size + g_slab_pages * PAGE;
  info.heap_used_size = used;
  info.heap_free_size =
      info.heap_total_size >= used ? info.heap_total_size - used : 0;
  info.heap_peak_size = peak_used_size;
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
void *aligned_alloc(size_t alignment, size_t size) {
  if (!alignment || (alignment & (alignment - 1)) != 0)
    return nullptr;
  if ((size & (alignment - 1)) != 0)
    return nullptr;
  if (alignment <= 16)
    return Kernel::Memory::g_kheap.kmalloc(size);
  if (alignment <= 4096 && size >= 128 * 1024)
    return Kernel::Memory::g_kheap.kmalloc(size);
  return nullptr;
}
}

// new T[0] / new T must never return null.
void *operator new(size_t size) { return malloc(size ? size : 1); }
void *operator new[](size_t size) { return malloc(size ? size : 1); }
void operator delete(void *ptr) noexcept { free(ptr); }
void operator delete[](void *ptr) noexcept { free(ptr); }
void operator delete(void *ptr, size_t) noexcept { free(ptr); }
void operator delete[](void *ptr, size_t) noexcept { free(ptr); }
