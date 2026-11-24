#include <stdlib.h>
#include <string.h>

#if __STDC_VERSION__ < 199901L && !defined(restrict) && !defined(__restrict)
#define __restrict
#endif

size_t strlen(const char *s) {
  const char *p = s;
  while (*p)
    p++;
  return p - s;
}

size_t strnlen(const char *s, size_t maxlen) {
  size_t n = 0;
  while (n < maxlen && s[n])
    n++;
  return n;
}

char *strcpy(char *__restrict dest, const char *__restrict src) {
  char *d = dest;
  while ((*d++ = *src++))
    ;
  return dest;
}

char *strncpy(char *__restrict dest, const char *__restrict src, size_t n) {
  size_t i = 0;
  while (i < n && src[i] != '\0') {
    dest[i] = src[i];
    i++;
  }
  while (i < n)
    dest[i++] = '\0';
  return dest;
}

char *strcat(char *__restrict dest, const char *__restrict src) {
  strcpy(dest + strlen(dest), src);
  return dest;
}

char *strncat(char *__restrict dest, const char *__restrict src, size_t n) {
  char *d = dest + strlen(dest);
  size_t i = 0;
  while (i < n && src[i] != '\0')
    *d++ = src[i++];
  *d = '\0';
  return dest;
}

int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
  if (n == 0)
    return 0;
  while (--n && *s1 && *s1 == *s2) {
    s1++;
    s2++;
  }
  return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static inline int ascii_tolower(int c) {
  return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

char *strdup(const char *s) {
  size_t len = strlen(s) + 1;
  char *d = (char *)malloc(len);
  if (d)
    memcpy(d, s, len);
  return d;
}

int strcasecmp(const char *s1, const char *s2) {
  int c1, c2;
  do {
    c1 = ascii_tolower((unsigned char)*s1++);
    c2 = ascii_tolower((unsigned char)*s2++);
    if (c1 == 0)
      break;
  } while (c1 == c2);
  return c1 - c2;
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
  int c1 = 0, c2 = 0;
  while (n > 0) {
    c1 = ascii_tolower((unsigned char)*s1++);
    c2 = ascii_tolower((unsigned char)*s2++);
    if (c1 == 0 || c1 != c2)
      break;
    n--;
  }
  return (n == 0) ? 0 : c1 - c2;
}

char *strchr(const char *s, int c) {
  unsigned char cc = (unsigned char)c;
  while (*s) {
    if (*s == cc)
      return (char *)s;
    s++;
  }
  return (cc == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
  unsigned char cc = (unsigned char)c;
  const char *last = NULL;
  do {
    if (*s == cc)
      last = s;
  } while (*s++);
  return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;
  size_t nlen = strlen(needle);
  while (*haystack) {
    if (*haystack == *needle && memcmp(haystack, needle, nlen) == 0)
      return (char *)haystack;
    haystack++;
  }
  return NULL;
}

size_t strspn(const char *s, const char *accept) {
  const char *p = s;
  while (*p && strchr(accept, *p))
    p++;
  return p - s;
}

size_t strcspn(const char *s, const char *reject) {
  const char *p = s;
  while (*p && !strchr(reject, *p))
    p++;
  return p - s;
}

char *strtok_r(char *__restrict s, const char *__restrict delim,
               char **__restrict saveptr) {
  if (s == NULL) {
    s = *saveptr;
    if (s == NULL)
      return NULL;
  }
  s += strspn(s, delim);
  if (*s == '\0') {
    *saveptr = NULL;
    return NULL;
  }
  char *token = s;
  s += strcspn(s, delim);
  if (*s == '\0') {
    *saveptr = NULL;
  } else {
    *s = '\0';
    *saveptr = s + 1;
  }
  return token;
}

char *strpbrk(const char *s, const char *accept) {
  while (*s) {
    if (strchr(accept, *s))
      return (char *)s;
    s++;
  }
  return NULL;
}

char *strtok(char *__restrict s, const char *__restrict delim) {
  static char *saveptr = NULL;
  return strtok_r(s, delim, &saveptr);
}

char *strerror(int errnum) {
  (void)errnum;
  static char buf[] = "kernel error";
  return buf;
}

int strcoll(const char *s1, const char *s2) { return strcmp(s1, s2); }

size_t strxfrm(char *__restrict dest, const char *__restrict src, size_t n) {
  size_t len = strlen(src);
  if (len < n) {
    memcpy(dest, src, len + 1);
  } else if (n > 0) {
    memcpy(dest, src, n - 1);
    dest[n - 1] = '\0';
  }
  return len;
}
