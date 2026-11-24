#define BUILDING_KERNEL_MEMORY
#include <stdint.h>
#include <string.h>

#define READ_64(p) (*(const uint64_t *)(p))

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
