#include <arch/x86_64/string.h>

extern void *malloc(size_t size);

#if __STDC_VERSION__ < 199901L && !defined(restrict) && !defined(__restrict)
#define __restrict
#endif

static inline int ascii_tolower(int c) {
  return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

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

char *stpcpy(char *__restrict dest, const char *__restrict src) {
  char *d = dest;
  while ((*d++ = *src++) != '\0')
    ;
  return d - 1;
}

char *stpncpy(char *__restrict dest, const char *__restrict src, size_t n) {
  size_t i = 0;
  while (i < n && src[i] != '\0') {
    dest[i] = src[i];
    i++;
  }
  char *ret = dest + i;
  while (i < n)
    dest[i++] = '\0';
  return ret;
}

char *strdup(const char *s) {
  size_t len = strlen(s) + 1;
  char *d = (char *)malloc(len);
  if (d)
    memcpy(d, s, len);
  return d;
}

char *strndup(const char *s, size_t n) {
  size_t len = strnlen(s, n);
  char *d = (char *)malloc(len + 1);
  if (d) {
    memcpy(d, s, len);
    d[len] = '\0';
  }
  return d;
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

char *strcasestr(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;
  size_t nlen = strlen(needle);
  while (*haystack) {
    if (ascii_tolower((unsigned char)*haystack) ==
        ascii_tolower((unsigned char)*needle)) {
      size_t i = 0;
      while (i < nlen && ascii_tolower((unsigned char)haystack[i]) ==
                             ascii_tolower((unsigned char)needle[i]))
        i++;
      if (i == nlen)
        return (char *)haystack;
    }
    haystack++;
  }
  return NULL;
}

char *strpbrk(const char *s, const char *accept) {
  while (*s) {
    if (strchr(accept, *s))
      return (char *)s;
    s++;
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

char *strtok(char *__restrict s, const char *__restrict delim) {
  static char *saveptr = NULL;
  return strtok_r(s, delim, &saveptr);
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

static void format_unknown(const char *prefix, int num, char *buf,
                           size_t maxlen) {
  size_t i = 0;
  while (prefix[i] && i < maxlen - 1) {
    buf[i] = prefix[i];
    i++;
  }
  char num_buf[32];
  int num_idx = 0;
  unsigned int unum = (num < 0) ? 0u - (unsigned int)num : (unsigned int)num;

  do {
    num_buf[num_idx++] = '0' + (unum % 10);
    unum /= 10;
  } while (unum > 0 && num_idx < (int)sizeof(num_buf));

  if (num < 0 && i < maxlen - 1)
    buf[i++] = '-';
  while (num_idx > 0 && i < maxlen - 1)
    buf[i++] = num_buf[--num_idx];
  buf[i] = '\0';
}

char *strerror(int errnum) {
  static char buf[64];
  format_unknown("Unknown error ", errnum, buf, sizeof(buf));
  return buf;
}

char *strsignal(int signum) {
  static char buf[64];
  format_unknown("Unknown signal ", signum, buf, sizeof(buf));
  return buf;
}

char *strsep(char **stringp, const char *delim) {
  char *s = *stringp;
  if (!s)
    return NULL;
  char *token = s;
  while (*s && !strchr(delim, *s))
    s++;
  if (*s) {
    *s++ = '\0';
    *stringp = s;
  } else {
    *stringp = NULL;
  }
  return token;
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