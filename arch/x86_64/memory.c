#define BUILDING_KERNEL_MEMORY
#include "arch/x86_64/string.h"

#define READ_64(p) (*(const uint64_t *)(p))
#define READ_32(p) (*(const uint32_t *)(p))

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
