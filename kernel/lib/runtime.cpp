#include <kernel/fs/laziofs.h>
#include <kernel/scheduler.h>
#include <limits.h>
#include <stdint.h>

namespace {
int bootstrap_errno = 0;
}

extern "C" {
int *__errno_location(void) {
  int *value = Kernel::Scheduler::current_errno_location();
  return value ? value : &bootstrap_errno;
}

static const uint16_t ctype_b[384] = {0};
static const int32_t ctype_toupper[384] = {0};
const uint16_t **__ctype_b_loc(void) {
  static const uint16_t *ptr = ctype_b + 128;
  return &ptr;
}
const int32_t **__ctype_toupper_loc(void) {
  static const int32_t *ptr = ctype_toupper + 128;
  return &ptr;
}
}

#define errno_val (*__errno_location())

namespace {
bool is_space_char(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}
}

extern "C" {
long strtol(const char *nptr, char **endptr, int base) {
  if (!nptr) {
    errno_val = 22;
    if (endptr)
      *endptr = nullptr;
    return 0;
  }
  const char *s = nptr;
  while (is_space_char(*s))
    ++s;
  bool negative = false;
  if (*s == '+' || *s == '-') {
    negative = *s == '-';
    ++s;
  }
  if (base == 0 || base == 16) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      base = 16;
      s += 2;
    }
  }
  if (base == 0)
    base = (*s == '0') ? 8 : 10;
  if (base < 2 || base > 36) {
    errno_val = 22;
    if (endptr)
      *endptr = const_cast<char *>(nptr);
    return 0;
  }
  unsigned long long value = 0;
  bool any = false;
  bool overflow = false;
  const unsigned long long limit =
      negative ? (static_cast<unsigned long long>(LONG_MAX) + 1ull)
               : static_cast<unsigned long long>(LONG_MAX);
  while (*s) {
    unsigned digit;
    if (*s >= '0' && *s <= '9')
      digit = *s - '0';
    else if (*s >= 'a' && *s <= 'z')
      digit = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'Z')
      digit = *s - 'A' + 10;
    else
      break;
    if (digit >= static_cast<unsigned>(base))
      break;
    any = true;
    if (value > (limit - digit) / static_cast<unsigned>(base))
      overflow = true;
    else if (!overflow)
      value = value * static_cast<unsigned>(base) + digit;
    ++s;
  }
  if (!any) {
    if (endptr)
      *endptr = const_cast<char *>(nptr);
    return 0;
  }
  if (endptr)
    *endptr = const_cast<char *>(s);
  if (overflow) {
    errno_val = 34;
    return negative ? LONG_MIN : LONG_MAX;
  }
  if (negative)
    return value == static_cast<unsigned long long>(LONG_MAX) + 1ull
               ? LONG_MIN
               : -static_cast<long>(value);
  return static_cast<long>(value);
}
double strtod(const char *nptr, char **endptr) {
  return static_cast<double>(strtol(nptr, endptr, 10));
}
int system(const char *command) {
  (void)command;
  errno_val = 38;
  return -1;
}
int mkdir(const char *pathname, int mode) {
  return Kernel::FS::VFS::vfs_mkdir(pathname, mode);
}
int remove(const char *pathname) {
  return Kernel::FS::VFS::vfs_remove(pathname);
}
int rename(const char *oldpath, const char *newpath) {
  return Kernel::FS::VFS::vfs_rename(oldpath, newpath);
}
long atol(const char *nptr) { return strtol(nptr, nullptr, 10); }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c; }
double fabs(double x) { return (x < 0.0) ? -x : x; }
}

