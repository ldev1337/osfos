
#include <kernel/fs/vfs.h>
#include <kernel/shell/shell.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

typedef struct __FILE {
    void* stream;
    bool is_stdout;
    bool is_stderr;
} FILE;

extern "C" {
static FILE __stdout_file = { nullptr, true, false };
static FILE __stderr_file = { nullptr, false, true };
FILE* stdout = &__stdout_file;
FILE* stderr = &__stderr_file;

static int errno_val = 0;
int* __errno_location(void) {
    return &errno_val;
}

static const uint16_t ctype_b[384] = { 0 };
static const int32_t ctype_toupper[384] = { 0 };

const uint16_t** __ctype_b_loc(void) {
    static const uint16_t* ptr = ctype_b + 128;
    return &ptr;
}

const int32_t** __ctype_toupper_loc(void) {
    static const int32_t* ptr = ctype_toupper + 128;
    return &ptr;
}

static inline void buf_putc(char* str, size_t size, size_t& pos, char c) {
    if (str != nullptr && pos + 1 < size) {
        str[pos] = c;
        str[pos + 1] = '\0';
    }
    pos++;
}

static inline void buf_puts(char* str, size_t size, size_t& pos, const char* s) {
    if (!s) s = "(null)";
    while (*s) {
        buf_putc(str, size, pos, *s++);
    }
}

static inline void buf_putint(char* str, size_t size, size_t& pos, long long val, int base, bool uppercase, bool signed_val, int width, char pad_char) {
    char temp[64];
    int i = 0;
    bool negative = false;

    if (signed_val && val < 0) {
        negative = true;
        val = -val;
    }

    if (val == 0) {
        temp[i++] = '0';
    } else {
        unsigned long long uval = static_cast<unsigned long long>(val);
        while (uval > 0) {
            unsigned long long rem = uval % base;
            temp[i++] = (rem < 10) ? ('0' + static_cast<char>(rem)) : ((uppercase ? 'A' : 'a') + static_cast<char>(rem - 10));
            uval /= base;
        }
    }

    int num_chars = i + (negative ? 1 : 0);

    if (pad_char == '0') {
        if (negative) {
            buf_putc(str, size, pos, '-');
            width--;
        }
        while (width > i) {
            buf_putc(str, size, pos, '0');
            width--;
        }
        while (i > 0) {
            buf_putc(str, size, pos, temp[--i]);
        }
    } else {
        int total_len = (num_chars > width) ? num_chars : width;
        int padding = total_len - num_chars;
        while (padding > 0) {
            buf_putc(str, size, pos, ' ');
            padding--;
        }
        if (negative) {
            buf_putc(str, size, pos, '-');
        }
        while (i > 0) {
            buf_putc(str, size, pos, temp[--i]);
        }
    }
}

int vsnprintf(char* str, size_t size, const char* format, va_list ap) {
    size_t pos = 0;
    if (str && size > 0) {
        str[0] = '\0';
    }

    const char* fmt = format;
    while (*fmt) {
        if (*fmt != '%') {
            buf_putc(str, size, pos, *fmt++);
            continue;
        }

        fmt++;
        if (!*fmt) break;

        char pad_char = ' ';
        if (*fmt == '0') {
            pad_char = '0';
            fmt++;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        bool is_long = false;
        bool is_long_long = false;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') {
                is_long_long = true;
                fmt++;
            } else {
                is_long = true;
            }
        }

        if (!*fmt) break;

        switch (*fmt) {
            case 's': {
                const char* s = va_arg(ap, const char*);
                buf_puts(str, size, pos, s);
                break;
            }
            case 'd':
            case 'i': {
                long long val = is_long_long ? va_arg(ap, long long) : (is_long ? va_arg(ap, long) : va_arg(ap, int));
                int eff_width = width;
                char eff_pad = pad_char;
                if (precision >= 0) {
                    if (precision > eff_width) eff_width = precision;
                    eff_pad = '0';
                }
                buf_putint(str, size, pos, val, 10, false, true, eff_width, eff_pad);
                break;
            }
            case 'u': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) : (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                int eff_width = width;
                char eff_pad = pad_char;
                if (precision >= 0) {
                    if (precision > eff_width) eff_width = precision;
                    eff_pad = '0';
                }
                buf_putint(str, size, pos, static_cast<long long>(val), 10, false, false, eff_width, eff_pad);
                break;
            }
            case 'x': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) : (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                int eff_width = width;
                char eff_pad = pad_char;
                if (precision >= 0) {
                    if (precision > eff_width) eff_width = precision;
                    eff_pad = '0';
                }
                buf_putint(str, size, pos, static_cast<long long>(val), 16, false, false, eff_width, eff_pad);
                break;
            }
            case 'X': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) : (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                int eff_width = width;
                char eff_pad = pad_char;
                if (precision >= 0) {
                    if (precision > eff_width) eff_width = precision;
                    eff_pad = '0';
                }
                buf_putint(str, size, pos, static_cast<long long>(val), 16, true, false, eff_width, eff_pad);
                break;
            }
            default:
                buf_putc(str, size, pos, '%');
                buf_putc(str, size, pos, *fmt);
                break;
        }
        fmt++;
    }

    if (str && size > 0) {
        if (pos < size) {
            str[pos] = '\0';
        } else {
            str[size - 1] = '\0';
        }
    }
    return static_cast<int>(pos);
}

int snprintf(char* str, size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(str, size, fmt, args);
    va_end(args);
    return ret;
}

int printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    int ret = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    kernel_printf("%s", buf);
    return ret;
}

int vfprintf(FILE* stream, const char* fmt, va_list ap) {
    char buf[512];
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (!stream || stream->is_stdout || stream->is_stderr) {
        kernel_printf("%s", buf);
    } else if (stream->stream) {
        Kernel::FS::VFS::vfs_write(buf, 1, ret, stream->stream);
    }
    return ret;
}

int fprintf(FILE* stream, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vfprintf(stream, fmt, args);
    va_end(args);
    return ret;
}

int puts(const char* s) {
    kernel_printf("%s\n", s ? s : "(null)");
    return 0;
}

int putchar(int c) {
    char buf[2] = {static_cast<char>(c), '\0'};
    kernel_printf("%s", buf);
    return c;
}

int putc(int c, FILE* stream) {
    if (stream && stream->stream) {
        unsigned char uc = static_cast<unsigned char>(c);
        Kernel::FS::VFS::vfs_write(&uc, 1, 1, stream->stream);
        return c;
    }
    return putchar(c);
}

int fflush(FILE* stream) {
    (void)stream;
    return 0;
}

FILE* fopen(const char* filename, const char* mode) {
    (void)mode;
    void* handle = Kernel::FS::VFS::vfs_open(filename);
    if (!handle) {
        errno_val = 2;
        return nullptr;
    }
    FILE* f = new FILE{handle, false, false};
    return f;
}

int fclose(FILE* stream) {
    if (!stream || stream->is_stdout || stream->is_stderr) {
        errno_val = 9;
        return -1;
    }
    int res = Kernel::FS::VFS::vfs_close(stream->stream);
    delete stream;
    return res;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream || stream->is_stdout || stream->is_stderr) {
        errno_val = 9;
        return 0;
    }
    return Kernel::FS::VFS::vfs_read(ptr, size, nmemb, stream->stream);
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream) {
        errno_val = 9;
        return 0;
    }
    if (stream->is_stdout || stream->is_stderr) {
        size_t total = size * nmemb;
        const char* cptr = static_cast<const char*>(ptr);
        for (size_t i = 0; i < total; i++) {
            putchar(cptr[i]);
        }
        return nmemb;
    }
    return Kernel::FS::VFS::vfs_write(ptr, size, nmemb, stream->stream);
}

int fseek(FILE* stream, long offset, int whence) {
    if (!stream || stream->is_stdout || stream->is_stderr) {
        errno_val = 9;
        return -1;
    }
    return Kernel::FS::VFS::vfs_seek(stream->stream, offset, whence);
}

long ftell(FILE* stream) {
    if (!stream || stream->is_stdout || stream->is_stderr) {
        errno_val = 9;
        return -1;
    }
    return Kernel::FS::VFS::vfs_tell(stream->stream);
}

int abs(int j) {
    return (j < 0) ? -j : j;
}

double fabs(double x) {
    return (x < 0.0) ? -x : x;
}

long strtol(const char* nptr, char** endptr, int base) {
    const char* s = nptr;
    unsigned long acc;
    int c;
    unsigned long cutoff;
    int neg = 0, any, cutlim;

    do {
        c = *s++;
    } while (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');

    if (c == '-') {
        neg = 1;
        c = *s++;
    } else if (c == '+') {
        c = *s++;
    }

    if ((base == 0 || base == 16) && c == '0' && (*s == 'x' || *s == 'X')) {
        c = s[1];
        s += 2;
        base = 16;
    }
    if (base == 0) {
        base = c == '0' ? 8 : 10;
    }

    cutoff = neg ? static_cast<unsigned long>(-(LONG_MIN)) : static_cast<unsigned long>(LONG_MAX);
    cutlim = cutoff % static_cast<unsigned long>(base);
    cutoff /= static_cast<unsigned long>(base);
    for (acc = 0, any = 0;; c = *s++) {
        if (c >= '0' && c <= '9')
            c -= '0';
        else if (c >= 'a' && c <= 'z')
            c -= 'a' - 10;
        else if (c >= 'A' && c <= 'Z')
            c -= 'A' - 10;
        else
            break;
        if (c >= base)
            break;
        if (any < 0)
            continue;
        if (acc > cutoff || (acc == cutoff && c > static_cast<unsigned long>(cutlim))) {
            any = -1;
            acc = neg ? static_cast<unsigned long>(LONG_MIN) : static_cast<unsigned long>(LONG_MAX);
            errno_val = 34;
        } else {
            any = 1;
            acc *= base;
            acc += c;
        }
    }
    if (any < 0) {
        acc = neg ? static_cast<unsigned long>(LONG_MIN) : static_cast<unsigned long>(LONG_MAX);
    } else if (neg) {
        acc = -acc;
    }
    if (endptr != nullptr) {
        *endptr = const_cast<char*>(any ? s - 1 : nptr);
    }
    return static_cast<long>(acc);
}

double strtod(const char* nptr, char** endptr) {
    return static_cast<double>(strtol(nptr, endptr, 10));
}

void exit(int status) {
    kernel_printf("\nProcess exited with status %d\n", status);
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

int system(const char* command) {
    (void)command;
    errno_val = 38;
    return -1;
}

int mkdir(const char* pathname, int mode) {
    return Kernel::FS::VFS::vfs_mkdir(pathname, mode);
}

int remove(const char* pathname) {
    return Kernel::FS::VFS::vfs_remove(pathname);
}

int rename(const char* oldpath, const char* newpath) {
    return Kernel::FS::VFS::vfs_rename(oldpath, newpath);
}

int vscanf_internal(const char* str, const char* format, va_list ap) {
    if (!str || !format) return 0;
    int assigned = 0;
    while (*format && *str) {
        if (*format == '%') {
            format++;
            if (*format == 'd' || *format == 'i') {
                int* val = va_arg(ap, int*);
                char* endptr;
                *val = static_cast<int>(strtol(str, &endptr, 10));
                if (endptr != str) {
                    assigned++;
                    str = endptr;
                } else {
                    break;
                }
            } else if (*format == 's') {
                char* dest = va_arg(ap, char*);
                while (*str && *str != ' ' && *str != '\t' && *str != '\n') {
                    *dest++ = *str++;
                }
                *dest = '\0';
                assigned++;
            }
        } else {
            if (*str == *format) {
                str++;
            }
        }
        format++;
    }
    return assigned;
}

int __isoc99_sscanf(const char* str, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vscanf_internal(str, format, args);
    va_end(args);
    return ret;
}

int sscanf(const char* str, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vscanf_internal(str, format, args);
    va_end(args);
    return ret;
}

int atoi(const char* nptr) {
    return static_cast<int>(strtol(nptr, nullptr, 10));
}

long atol(const char* nptr) {
    return strtol(nptr, nullptr, 10);
}
int toupper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}
}