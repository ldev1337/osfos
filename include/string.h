#ifndef KERNEL_STRING_H
#define KERNEL_STRING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((always_inline)) static inline void *
memcpy(void *__restrict d, const void *__restrict s, size_t sz) {
  return __builtin_memcpy(d, s, sz);
}

__attribute__((always_inline)) static inline void *memset(void *d, int c,
                                                          size_t sz) {
  return __builtin_memset(d, c, sz);
}

__attribute__((always_inline)) static inline void *
memmove(void *d, const void *s, size_t sz) {
  return __builtin_memmove(d, s, sz);
}

__attribute__((always_inline)) static inline int
memcmp(const void *s1, const void *s2, size_t sz) {
  return __builtin_memcmp(s1, s2, sz);
}

void *memchr(const void *s, int c, size_t n);

void *memrchr(const void *s, int c, size_t n);
void *memccpy(void *__restrict dest, const void *__restrict src, int c,
              size_t n);
void *rawmemchr(const void *s, int c);
void *explicit_memset(void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);

char *strcpy(char *__restrict dest, const char *__restrict src);
char *strncpy(char *__restrict dest, const char *__restrict src, size_t n);
char *stpcpy(char *__restrict dest, const char *__restrict src);
char *stpncpy(char *__restrict dest, const char *__restrict src, size_t n);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);

char *strcat(char *__restrict dest, const char *__restrict src);
char *strncat(char *__restrict dest, const char *__restrict src, size_t n);

int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strcasestr(const char *haystack, const char *needle);
char *strpbrk(const char *s, const char *accept);

size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);

char *strtok(char *__restrict s, const char *__restrict delim);
char *strtok_r(char *__restrict s, const char *__restrict delim,
               char **__restrict saveptr);

char *strerror(int errnum);
char *strsignal(int signum);
char *strsep(char **stringp, const char *delim);

int strcoll(const char *s1, const char *s2);
size_t strxfrm(char *__restrict dest, const char *__restrict src, size_t n);

#ifdef __cplusplus
}
#endif

#endif