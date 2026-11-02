#define BUILDING_KERNEL_MEMORY
#include "arch/x86_64/string.h"

#define READ_64(p) (*(const uint64_t *)(p))
#define READ_32(p) (*(const uint32_t *)(p))

void *memset(void *d, int c, size_t sz) {
  void *ret = d;
  __asm__ volatile("rep stosb" : "+D"(d), "+c"(sz) : "a"(c) : "memory");
  return ret;
}

void *memcpy(void *__restrict d, const void *__restrict s, size_t sz) {
  unsigned char *dest = (unsigned char *)d;
  const unsigned char *src = (const unsigned char *)s;

  while (sz && ((uintptr_t)dest & (sizeof(uint64_t) - 1))) {
    *dest++ = *src++;
    --sz;
  }
  while (sz >= sizeof(uint64_t)) {
    *(uint64_t *)dest = *(const uint64_t *)src;
    dest += sizeof(uint64_t);
    src += sizeof(uint64_t);
    sz -= sizeof(uint64_t);
  }
  while (sz--)
    *dest++ = *src++;
  return d;
}

void *memmove(void *d, const void *s, size_t sz) {
  unsigned char *dst = (unsigned char *)d;
  const unsigned char *src = (const unsigned char *)s;
  if (dst < src) {
    for (size_t i = 0; i < sz; i++) {
      dst[i] = src[i];
    }
  } else if (dst > src) {
    for (size_t i = sz; i > 0; i--) {
      dst[i - 1] = src[i - 1];
    }
  }
  return d;
}

int memcmp(const void *s1, const void *s2, size_t sz) {
  const unsigned char *p1 = s1;
  const unsigned char *p2 = s2;
  while (sz >= 8) {
    uint64_t v1 = *(const uint64_t *)p1;
    uint64_t v2 = *(const uint64_t *)p2;
    if (v1 != v2) {
      uint64_t diff = v1 ^ v2;
      int shift = __builtin_ctzll(diff) & ~7;
      unsigned char b1 = (v1 >> shift) & 0xFF;
      unsigned char b2 = (v2 >> shift) & 0xFF;
      return (b1 < b2) ? -1 : 1;
    }
    p1 += 8;
    p2 += 8;
    sz -= 8;
  }
  while (sz > 0) {
    if (*p1 != *p2) {
      return (*p1 < *p2) ? -1 : 1;
    }
    p1++;
    p2++;
    sz--;
  }
  return 0;
}

void *memchr(const void *s, int c, size_t sz) {
  const unsigned char *src = (const unsigned char *)s;
  unsigned char ch = (unsigned char)c;
  uint64_t mask = ch;
  mask |= mask << 8;
  mask |= mask << 16;
  mask |= mask << 32;

  while (sz >= 8) {
    uint64_t val = READ_64(src) ^ mask;
    if (((val - 0x0101010101010101ULL) & ~val & 0x8080808080808080ULL) != 0) {
      for (size_t i = 0; i < 8; i++) {
        if (src[i] == ch)
          return (void *)(src + i);
      }
    }
    src += 8;
    sz -= 8;
  }
  while (sz > 0) {
    if (*src == ch)
      return (void *)src;
    src++;
    sz--;
  }
  return NULL;
}

void *memrchr(const void *s, int c, size_t sz) {
  const unsigned char *src = (const unsigned char *)s;
  unsigned char ch = (unsigned char)c;
  if (sz == 0)
    return NULL;

  uint64_t mask = ch;
  mask |= mask << 8;
  mask |= mask << 16;
  mask |= mask << 32;

  const unsigned char *p = src + sz;
  while (sz >= 8) {
    p -= 8;
    sz -= 8;
    uint64_t val = READ_64(p) ^ mask;
    if (((val - 0x0101010101010101ULL) & ~val & 0x8080808080808080ULL) != 0) {
      for (int i = 7; i >= 0; i--) {
        if (p[i] == ch)
          return (void *)(p + i);
      }
    }
  }
  while (sz > 0) {
    p--;
    sz--;
    if (*p == ch)
      return (void *)p;
  }
  return NULL;
}

void *explicit_memset(void *s, int c, size_t n) {
  volatile unsigned char *p = s;
  while (n--) {
    *p++ = (unsigned char)c;
  }
#if defined(__GNUC__) || defined(__clang__)
  __asm__ __volatile__("" : : "r"(s) : "memory");
#elif defined(_MSC_VER)
  _ReadWriteBarrier();
#endif
  return s;
}

void *rawmemchr(const void *s, int c) {
  const unsigned char *p = s;
  unsigned char uc = c;

  while ((uintptr_t)p & (sizeof(uintptr_t) - 1))
    if (*p == uc)
      return (void *)p;
    else
      p++;

  uintptr_t ones = UINTPTR_MAX / 255;
  uintptr_t mask = uc * ones;
  uintptr_t highs = ones * 128;

  const uintptr_t *w = (const uintptr_t *)p;
  while (1) {
    uintptr_t word = *w ^ mask;
    if ((word - ones) & ~word & highs) {
      p = (const unsigned char *)w;
      for (size_t i = 0; i < sizeof(uintptr_t); ++i)
        if (p[i] == uc)
          return (void *)(p + i);
    }
    w++;
  }
}

void *memccpy(void *__restrict dest, const void *__restrict src, int c,
              size_t n) {
  unsigned char *d = dest;
  const unsigned char *s = src;
  unsigned char uc = (unsigned char)c;

  while (n > 0) {
    unsigned char val = *s++;
    *d++ = val;
    n--;
    if (val == uc) {
      return d;
    }
  }
  return NULL;
}
