#ifndef _OSFOS_STDIO_H
#define _OSFOS_STDIO_H
#include <stdarg.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct __FILE FILE;
typedef long fpos_t;
#ifndef EOF
#define EOF (-1)
#endif
#ifndef BUFSIZ
#define BUFSIZ 512
#endif
#ifndef FILENAME_MAX
#define FILENAME_MAX 256
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
FILE *__os_stdin(void);
FILE *__os_stdout(void);
FILE *__os_stderr(void);
#ifndef OSFOS_STDIO_IMPLEMENTATION
#define stdin (__os_stdin())
#define stdout (__os_stdout())
#define stderr (__os_stderr())
#endif
int remove(const char *filename);
int rename(const char *old_name, const char *new_name);
int printf(const char *format, ...);
int vprintf(const char *format, va_list args);
int fprintf(FILE *stream, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list args);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);
int sprintf(char *buffer, const char *format, ...);
int vsprintf(char *buffer, const char *format, va_list args);
int puts(const char *text);
int fputs(const char *text, FILE *stream);
int putchar(int character);
int fputc(int character, FILE *stream);
int putc(int character, FILE *stream);
int getchar(void);
int fgetc(FILE *stream);
int getc(FILE *stream);
char *fgets(char *buffer, int size, FILE *stream);
int ungetc(int character, FILE *stream);
FILE *fopen(const char *filename, const char *mode);
int fclose(FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int fflush(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int fgetpos(FILE *stream, fpos_t *position);
int fsetpos(FILE *stream, const fpos_t *position);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fileno(FILE *stream);
int isatty(int descriptor);
int setvbuf(FILE *stream, char *buffer, int mode, size_t size);
void setbuf(FILE *stream, char *buffer);
int scanf(const char *format, ...);
int vscanf(const char *format, va_list args);
int fscanf(FILE *stream, const char *format, ...);
int sscanf(const char *text, const char *format, ...);
int vsscanf(const char *text, const char *format, va_list args);
int vfscanf(FILE *stream, const char *format, va_list args);
void perror(const char *prefix);
#ifdef __cplusplus
}
#endif
#endif
