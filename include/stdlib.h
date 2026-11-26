#ifndef _OSFOS_STDLIB_H
#define _OSFOS_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void *)0)
#endif
#endif

#ifndef _NORETURN
#if defined(__cplusplus)
#define _NORETURN [[noreturn]]
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define _NORETURN _Noreturn
#else
#define _NORETURN __attribute__((noreturn))
#endif
#endif

/* malloc is process-lifetime memory for application processes. */
void *malloc(size_t size);
/* Thread-lifetime allocation; reclaimed automatically when this thread exits. */
void *thread_malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void *aligned_alloc(size_t alignment, size_t size);

_NORETURN void exit(int status);

int atoi(const char *nptr);
double atof(const char *nptr);

int abs(int j);

#ifdef __cplusplus
}
#endif

#endif /* _OSFOS_STDLIB_H */
