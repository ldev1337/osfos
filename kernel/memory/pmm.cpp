#include <arch/x86_64/spinlock.h>
#include <arch/x86_64/string.h>
#include <kernel/memory/pmm.h>

#include <algorithm>

namespace Kernel::Memory {

alignas(64) PMM g_pmm;
static Spinlock pmm_lock;

// Two-bit page states:
//
// bitmap  state_bitmap  Meaning
//   0          0        Usable and free
//   1          0        Usable and allocated
//   0          1        Usable but permanently reserved
//   1          1        Not yet registered / unavailable

namespace {

using word_t = uint64_t __attribute__((may_alias));

constexpr size_t HUGE_PAGES = 512; // 2 MiB / 4 KiB
constexpr size_t HUGE_WORDS = HUGE_PAGES / 64;

struct IrqGuard {
  uint64_t flags;
  IrqGuard() : flags(pmm_lock.lock_irqsave()) {}
  ~IrqGuard() { pmm_lock.unlock_irqrestore(flags); }
  IrqGuard(const IrqGuard &) = delete;
  IrqGuard &operator=(const IrqGuard &) = delete;
};

inline uint64_t range_mask(size_t lo, size_t hi) {
  const uint64_t top = (hi == 64) ? ~0ull : ((1ull << hi) - 1);
  return top & (~0ull << lo);
}

} // namespace

void PMM::set_bit(size_t page_index, bool value) {
  const size_t byte_index = page_index >> 3;
  const uint8_t mask = static_cast<uint8_t>(1u << (page_index & 7));
  if (value)
    bitmap[byte_index] |= mask;
  else
    bitmap[byte_index] &= static_cast<uint8_t>(~mask);
}

bool PMM::get_bit(size_t page_index) const {
  return (bitmap[page_index >> 3] >> (page_index & 7)) & 1;
}

void PMM::set_state_bit(size_t page_index, bool value) {
  const size_t byte_index = page_index >> 3;
  const uint8_t mask = static_cast<uint8_t>(1u << (page_index & 7));
  if (value)
    state_bitmap[byte_index] |= mask;
  else
    state_bitmap[byte_index] &= static_cast<uint8_t>(~mask);
}

bool PMM::get_state_bit(size_t page_index) const {
  return (state_bitmap[page_index >> 3] >> (page_index & 7)) & 1;
}

void PMM::init(void *bitmap_buffer, size_t mem_size) {
  IrqGuard guard;

  bitmap = nullptr;
  state_bitmap = nullptr;
  bitmap_pages = 0;
  total_pages = 0;
  bitmap_size = 0;
  used_pages_count = 0;
  bootstrap_open = false;
  hint_word = 0;

  if (__builtin_expect(!bitmap_buffer || mem_size == 0 ||
                           (reinterpret_cast<uintptr_t>(bitmap_buffer) & 7),
                       0))
    return;

  const size_t pages = mem_size / PAGE_SIZE + (mem_size % PAGE_SIZE != 0);
  const size_t words = pages / 64 + (pages % 64 != 0);

  if (__builtin_expect(words > (static_cast<size_t>(-1) / 16), 0))
    return;

  bitmap_pages = pages;
  bitmap_size = words * 8;
  bitmap = static_cast<uint8_t *>(bitmap_buffer);
  state_bitmap = bitmap + bitmap_size;
  memset(bitmap, 0xFF, bitmap_size * 2);

  bootstrap_open = true;
}

void PMM::reserve_page(size_t page_index) {
  IrqGuard guard;

  if (__builtin_expect(!bitmap || page_index >= bitmap_pages, 0))
    return;

  const size_t byte_index = page_index >> 3;
  const uint8_t mask = static_cast<uint8_t>(1u << (page_index & 7));
  const bool used = bitmap[byte_index] & mask;
  const bool state = state_bitmap[byte_index] & mask;

  if (used && state)
    return;

  if (!used && !state)
    ++used_pages_count;

  bitmap[byte_index] &= static_cast<uint8_t>(~mask);
  state_bitmap[byte_index] |= mask;
}

void *PMM::allocate_page_block(size_t count) {
  if (__builtin_expect(count == 0, 0))
    return nullptr;

  IrqGuard guard;

  if (__builtin_expect(!bitmap, 0))
    return nullptr;

  bootstrap_open = false;

  if (__builtin_expect(used_pages_count > total_pages ||
                           count > total_pages - used_pages_count,
                       0))
    return nullptr;

  word_t *const bm = reinterpret_cast<word_t *>(bitmap);
  const word_t *const sm = reinterpret_cast<const word_t *>(state_bitmap);
  const size_t words = bitmap_size >> 3;

  if (count == 1) {
    for (size_t w = hint_word; w < words; ++w) {
      const uint64_t free_mask = ~(bm[w] | sm[w]);
      if (free_mask) {
        const unsigned bit = __builtin_ctzll(free_mask);
        bm[w] |= 1ull << bit;
        ++used_pages_count;
        hint_word = w;
        return reinterpret_cast<void *>(
            static_cast<uintptr_t>(((w << 6) + bit) * PAGE_SIZE));
      }
    }
    hint_word = words; // nothing free anywhere
    return nullptr;
  }

  size_t run = 0;
  size_t start = 0;

  for (size_t w = hint_word; w < words; ++w) {
    const uint64_t f = ~(bm[w] | sm[w]);

    if (f == 0) {
      run = 0;
      continue;
    }

    size_t pos = 0;
    while (pos < 64) {
      const uint64_t rest = f >> pos;
      if (rest == 0) {
        run = 0;
        break;
      }

      const unsigned zeros = __builtin_ctzll(rest);
      if (zeros) {
        run = 0;
        pos += zeros;
      }

      const unsigned ones = __builtin_ctzll(~(f >> pos));
      if (run == 0)
        start = (w << 6) + pos;
      run += ones;
      pos += ones;

      if (run >= count) {
        const size_t end = start + count;
        size_t first = start >> 6;
        const size_t last = (end - 1) >> 6;
        for (; first <= last; ++first) {
          const size_t lo = (first == (start >> 6)) ? (start & 63) : 0;
          const size_t hi = (first == last) ? (((end - 1) & 63) + 1) : 64;
          bm[first] |= range_mask(lo, hi);
        }
        used_pages_count += count;
        return reinterpret_cast<void *>(
            static_cast<uintptr_t>(start * PAGE_SIZE));
      }
    }
  }

  return nullptr;
}

void PMM::free_page_block(void *ptr, size_t count) {
  if (__builtin_expect(count == 0, 0))
    return;

  const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
  if (__builtin_expect((address & (PAGE_SIZE - 1)) != 0, 0))
    return;

  IrqGuard guard;
  const size_t start_page = address / PAGE_SIZE;

  if (__builtin_expect(!bitmap || start_page >= bitmap_pages, 0))
    return;

  count = std::min(count, bitmap_pages - start_page);
  const size_t end_page = start_page + count;

  word_t *const bm = reinterpret_cast<word_t *>(bitmap);
  word_t *const sm = reinterpret_cast<word_t *>(state_bitmap);

  const size_t first_w = start_page >> 6;
  const size_t last_w = (end_page - 1) >> 6;

  size_t freed = 0; // 10 -> 00
  size_t newly = 0; // 11 -> 00 (bootstrap registration)

  for (size_t w = first_w; w <= last_w; ++w) {
    const size_t lo = (w == first_w) ? (start_page & 63) : 0;
    const size_t hi = (w == last_w) ? (((end_page - 1) & 63) + 1) : 64;
    const uint64_t m = range_mask(lo, hi);

    const uint64_t b = bm[w];
    const uint64_t s = sm[w];

    if (bootstrap_open) {
      const uint64_t f10 = b & ~s & m;
      const uint64_t f11 = b & s & m;
      bm[w] = b & ~m;
      sm[w] = s & ~f11;
      freed += __builtin_popcountll(f10);
      newly += __builtin_popcountll(f11);
    } else {
      const uint64_t t = b & ~s & m; // only allocated pages may be freed
      bm[w] = b & ~t;
      freed += __builtin_popcountll(t);
    }
  }

  used_pages_count -= std::min(used_pages_count, freed);
  total_pages = std::min(total_pages + newly, bitmap_pages);

  if ((freed | newly) && first_w < hint_word)
    hint_word = first_w;
}

size_t PMM::recheck_used_memory() const {
  IrqGuard guard;

  if (!bitmap)
    return 0;

  // States 01 and 10 count as used; 00 and 11 do not. That is bm XOR sm.
  const word_t *const bm = reinterpret_cast<const word_t *>(bitmap);
  const word_t *const sm = reinterpret_cast<const word_t *>(state_bitmap);
  const size_t words = bitmap_size >> 3;

  size_t used_pages = 0;
  for (size_t w = 0; w < words; ++w)
    used_pages += __builtin_popcountll(bm[w] ^ sm[w]);

  return used_pages * PAGE_SIZE;
}

void *PMM::allocate_huge_page() {
  IrqGuard guard;

  if (__builtin_expect(!bitmap, 0))
    return nullptr;

  bootstrap_open = false;

  if (__builtin_expect(used_pages_count > total_pages ||
                           HUGE_PAGES > total_pages - used_pages_count,
                       0))
    return nullptr;

  word_t *const bm = reinterpret_cast<word_t *>(bitmap);
  const word_t *const sm = reinterpret_cast<const word_t *>(state_bitmap);
  const size_t words = bitmap_size >> 3;

  for (size_t w = hint_word & ~(HUGE_WORDS - 1); w + HUGE_WORDS <= words;
       w += HUGE_WORDS) {
    uint64_t busy = 0;
    for (size_t k = 0; k < HUGE_WORDS; ++k)
      busy |= bm[w + k] | sm[w + k];

    if (busy == 0) {
      for (size_t k = 0; k < HUGE_WORDS; ++k)
        bm[w + k] = ~0ull;

      used_pages_count += HUGE_PAGES;
      return reinterpret_cast<void *>(
          static_cast<uintptr_t>((w << 6) * PAGE_SIZE));
    }
  }

  return nullptr;
}

} // namespace Kernel::Memory