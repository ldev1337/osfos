#include <kernel/fs/laziofs.h>
#include <kernel/programs/terminal.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct __FILE {
  void *stream;
  unsigned char is_stdout, is_stderr, is_stdin;
  unsigned char eof, error, unget_count;
  unsigned char unget_buffer[8];
  int descriptor;
};

extern "C" {
static FILE __stdin_file = {nullptr, 0, 0, 1, 0, 0, 0, {0}, 0};
static FILE __stdout_file = {nullptr, 1, 0, 0, 0, 0, 0, {0}, 1};
static FILE __stderr_file = {nullptr, 0, 1, 0, 0, 0, 0, {0}, 2};
FILE *stdin = &__stdin_file;
FILE *stdout = &__stdout_file;
FILE *stderr = &__stderr_file;
static int errno_val = 0;
static int next_descriptor = 3;
int *__errno_location(void) { return &errno_val; }

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

namespace {
namespace Terminal = Kernel::Programs::Terminal;
constexpr size_t STACK_FORMAT_SIZE = 512;

bool is_stdout_stream(FILE *stream) { return stream && stream->is_stdout; }
bool is_stderr_stream(FILE *stream) { return stream && stream->is_stderr; }
bool is_stdin_stream(FILE *stream) { return stream && stream->is_stdin; }
bool is_console_output(FILE *stream) {
  return is_stdout_stream(stream) || is_stderr_stream(stream);
}
bool is_file_stream(FILE *stream) {
  return stream && stream->stream && !is_console_output(stream) &&
         !is_stdin_stream(stream);
}
bool is_space_char(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

struct FormatBuffer {
  char *data;
  size_t capacity;
  size_t length;
  void put(char c) {
    if (data && capacity && length + 1 < capacity)
      data[length] = c;
    ++length;
  }
  void repeat(char c, size_t count) {
    while (count--)
      put(c);
  }
  void finish() {
    if (data && capacity)
      data[length < capacity ? length : capacity - 1] = '\0';
  }
};

enum class LengthMod : uint8_t { NONE, HH, H, L, LL, Z, T, J, BIG_L };

void emit_unsigned(FormatBuffer &out, unsigned long long value, unsigned base,
                   bool uppercase, bool negative, bool plus, bool space,
                   bool alternate, bool left, bool zero, int width,
                   int precision) {
  char digits[65];
  size_t count = 0;
  const char *alphabet = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
  if (value || precision != 0) {
    do {
      digits[count++] = alphabet[value % base];
      value /= base;
    } while (value && count < sizeof(digits));
  }
  const size_t zeros = precision > static_cast<int>(count)
                           ? static_cast<size_t>(precision - count)
                           : 0;
  char sign = negative ? '-' : (plus ? '+' : (space ? ' ' : 0));
  char prefix[2];
  size_t prefix_size = 0;
  if (alternate && base == 16 && count) {
    prefix[0] = '0';
    prefix[1] = uppercase ? 'X' : 'x';
    prefix_size = 2;
  } else if (alternate && base == 8 && (!count || digits[count - 1] != '0')) {
    prefix[0] = '0';
    prefix_size = 1;
  }
  size_t body = count + zeros + prefix_size + (sign ? 1 : 0);
  size_t padding =
      width > static_cast<int>(body) ? static_cast<size_t>(width - body) : 0;
  if (!left && (!zero || precision >= 0))
    out.repeat(' ', padding);
  if (sign)
    out.put(sign);
  for (size_t i = 0; i < prefix_size; ++i)
    out.put(prefix[i]);
  if (!left && zero && precision < 0)
    out.repeat('0', padding);
  out.repeat('0', zeros);
  while (count)
    out.put(digits[--count]);
  if (left)
    out.repeat(' ', padding);
}

void emit_string(FormatBuffer &out, const char *text, int width, int precision,
                 bool left) {
  if (!text)
    text = "(null)";
  size_t length = 0;
  while (text[length] &&
         (precision < 0 || length < static_cast<size_t>(precision)))
    ++length;
  const size_t padding = width > static_cast<int>(length)
                             ? static_cast<size_t>(width - length)
                             : 0;
  if (!left)
    out.repeat(' ', padding);
  for (size_t i = 0; i < length; ++i)
    out.put(text[i]);
  if (left)
    out.repeat(' ', padding);
}

int format_to_buffer(char *buffer, size_t size, const char *format,
                     va_list args) {
  if (!format)
    return -1;
  FormatBuffer out{buffer, size, 0};
  while (*format) {
    if (*format != '%') {
      out.put(*format++);
      continue;
    }
    ++format;
    if (*format == '%') {
      out.put('%');
      ++format;
      continue;
    }
    bool left = false, plus = false, space = false, alternate = false,
         zero = false;
    bool flags = true;
    while (flags) {
      switch (*format) {
      case '-':
        left = true;
        ++format;
        break;
      case '+':
        plus = true;
        ++format;
        break;
      case ' ':
        space = true;
        ++format;
        break;
      case '#':
        alternate = true;
        ++format;
        break;
      case '0':
        zero = true;
        ++format;
        break;
      default:
        flags = false;
        break;
      }
    }
    int width = 0;
    if (*format == '*') {
      width = va_arg(args, int);
      ++format;
      if (width < 0) {
        left = true;
        width = -width;
      }
    } else {
      while (*format >= '0' && *format <= '9') {
        if (width < 100000)
          width = width * 10 + (*format - '0');
        ++format;
      }
    }
    int precision = -1;
    if (*format == '.') {
      ++format;
      precision = 0;
      if (*format == '*') {
        precision = va_arg(args, int);
        ++format;
        if (precision < 0)
          precision = -1;
      } else
        while (*format >= '0' && *format <= '9') {
          if (precision < 100000)
            precision = precision * 10 + (*format - '0');
          ++format;
        }
    }
    LengthMod length = LengthMod::NONE;
    if (*format == 'h') {
      ++format;
      length = (*format == 'h') ? (++format, LengthMod::HH) : LengthMod::H;
    } else if (*format == 'l') {
      ++format;
      length = (*format == 'l') ? (++format, LengthMod::LL) : LengthMod::L;
    } else if (*format == 'z') {
      ++format;
      length = LengthMod::Z;
    } else if (*format == 't') {
      ++format;
      length = LengthMod::T;
    } else if (*format == 'j') {
      ++format;
      length = LengthMod::J;
    } else if (*format == 'L') {
      ++format;
      length = LengthMod::BIG_L;
    }
    const char conversion = *format ? *format++ : '\0';
    switch (conversion) {
    case 's':
      emit_string(out, va_arg(args, const char *), width, precision, left);
      break;
    case 'c': {
      const char c = static_cast<char>(va_arg(args, int));
      if (!left && width > 1)
        out.repeat(' ', width - 1);
      out.put(c);
      if (left && width > 1)
        out.repeat(' ', width - 1);
      break;
    }
    case 'd':
    case 'i': {
      long long value;
      if (length == LengthMod::LL || length == LengthMod::J)
        value = va_arg(args, long long);
      else if (length == LengthMod::L || length == LengthMod::T)
        value = va_arg(args, long);
      else if (length == LengthMod::Z)
        value = static_cast<long long>(va_arg(args, ptrdiff_t));
      else if (length == LengthMod::H)
        value = static_cast<short>(va_arg(args, int));
      else if (length == LengthMod::HH)
        value = static_cast<signed char>(va_arg(args, int));
      else
        value = va_arg(args, int);
      const bool neg = value < 0;
      const unsigned long long magnitude =
          neg ? 0ull - static_cast<unsigned long long>(value)
              : static_cast<unsigned long long>(value);
      emit_unsigned(out, magnitude, 10, false, neg, plus, space, false, left,
                    zero, width, precision);
      break;
    }
    case 'u':
    case 'o':
    case 'x':
    case 'X':
    case 'p': {
      unsigned long long value;
      unsigned base =
          conversion == 'o'
              ? 8u
              : ((conversion == 'x' || conversion == 'X' || conversion == 'p')
                     ? 16u
                     : 10u);
      if (conversion == 'p')
        value = reinterpret_cast<uintptr_t>(va_arg(args, void *));
      else if (length == LengthMod::LL || length == LengthMod::J)
        value = va_arg(args, unsigned long long);
      else if (length == LengthMod::L)
        value = va_arg(args, unsigned long);
      else if (length == LengthMod::Z)
        value = va_arg(args, size_t);
      else if (length == LengthMod::T)
        value = static_cast<unsigned long long>(va_arg(args, ptrdiff_t));
      else if (length == LengthMod::H)
        value = static_cast<unsigned short>(va_arg(args, unsigned int));
      else if (length == LengthMod::HH)
        value = static_cast<unsigned char>(va_arg(args, unsigned int));
      else
        value = va_arg(args, unsigned int);
      int p = precision;
      if (conversion == 'p' && p < static_cast<int>(sizeof(void *) * 2))
        p = sizeof(void *) * 2;
      emit_unsigned(out, value, base, conversion == 'X', false, false, false,
                    alternate || conversion == 'p', left, zero, width, p);
      break;
    }
    case 'n': {
      if (length == LengthMod::LL || length == LengthMod::J)
        *va_arg(args, long long *) = static_cast<long long>(out.length);
      else if (length == LengthMod::L)
        *va_arg(args, long *) = static_cast<long>(out.length);
      else if (length == LengthMod::Z)
        *va_arg(args, size_t *) = out.length;
      else if (length == LengthMod::H)
        *va_arg(args, short *) = static_cast<short>(out.length);
      else if (length == LengthMod::HH)
        *va_arg(args, signed char *) = static_cast<signed char>(out.length);
      else
        *va_arg(args, int *) = static_cast<int>(out.length);
      break;
    }
    case 'f':
    case 'F': {
      double value = va_arg(args, double);
      bool neg = value < 0.0;
      if (neg)
        value = -value;
      int p = precision < 0 ? 6 : (precision > 9 ? 9 : precision);
      unsigned long long scale = 1;
      for (int i = 0; i < p; ++i)
        scale *= 10;
      unsigned long long whole = value > 18446744073709547520.0
                                     ? 0ull
                                     : static_cast<unsigned long long>(value);
      double fractional = value - static_cast<double>(whole);
      unsigned long long fraction = static_cast<unsigned long long>(
          fractional * static_cast<double>(scale) + 0.5);
      if (fraction >= scale && scale) {
        ++whole;
        fraction = 0;
      }
      char number[96];
      size_t n = 0;
      do {
        number[n++] = static_cast<char>('0' + whole % 10);
        whole /= 10;
      } while (whole && n < sizeof(number) - 16);
      char tmp[128];
      size_t t = 0;
      while (n)
        tmp[t++] = number[--n];
      if (p || alternate)
        tmp[t++] = '.';
      if (p) {
        unsigned long long div = scale / 10;
        for (int i = 0; i < p; ++i) {
          tmp[t++] = static_cast<char>('0' + (fraction / div) % 10);
          div /= 10;
        }
      }
      tmp[t] = '\0';
      const size_t total = t + (neg || plus || space ? 1 : 0);
      const size_t pad = width > static_cast<int>(total)
                             ? static_cast<size_t>(width - total)
                             : 0;
      if (!left && !zero)
        out.repeat(' ', pad);
      if (neg)
        out.put('-');
      else if (plus)
        out.put('+');
      else if (space)
        out.put(' ');
      if (!left && zero)
        out.repeat('0', pad);
      for (size_t i = 0; i < t; ++i)
        out.put(tmp[i]);
      if (left)
        out.repeat(' ', pad);
      break;
    }
    case '\0':
      out.put('%');
      break;
    default:
      out.put('%');
      if (conversion)
        out.put(conversion);
      break;
    }
  }
  out.finish();
  return out.length > static_cast<size_t>(INT_MAX)
             ? -1
             : static_cast<int>(out.length);
}

int stream_write(FILE *stream, const void *buffer, size_t bytes) {
  if (!stream || (!buffer && bytes)) {
    errno_val = 9;
    return -1;
  }
  if (!bytes)
    return 0;
  if (is_stdout_stream(stream) || is_stderr_stream(stream)) {
    const uint32_t color = is_stderr_stream(stream) ? 0xF38BA8u : 0xF4D6CDu;
    Terminal::write(static_cast<const char *>(buffer), bytes, color);
    Terminal::flush();
    return static_cast<int>(bytes);
  }
  if (is_file_stream(stream)) {
    const size_t written =
        Kernel::FS::VFS::vfs_write(buffer, 1, bytes, stream->stream);
    if (written != bytes)
      stream->error = 1;
    return written > static_cast<size_t>(INT_MAX) ? INT_MAX
                                                  : static_cast<int>(written);
  }
  errno_val = 9;
  stream->error = 1;
  return -1;
}

int stream_get(FILE *stream) {
  if (!stream) {
    errno_val = 9;
    return EOF;
  }
  if (stream->unget_count)
    return stream->unget_buffer[--stream->unget_count];
  if (is_stdin_stream(stream)) {
    return Terminal::read_char();
  }
  if (is_file_stream(stream)) {
    unsigned char ch = 0;
    if (Kernel::FS::VFS::vfs_read(&ch, 1, 1, stream->stream) == 1)
      return ch;
    stream->eof = 1;
    return EOF;
  }
  errno_val = 9;
  stream->error = 1;
  return EOF;
}

struct ScanInput {
  FILE *file;
  const char *text;
  size_t position;
  size_t consumed;
};
int scan_get(ScanInput &input) {
  int c;
  if (input.file)
    c = stream_get(input.file);
  else
    c = input.text[input.position]
            ? static_cast<unsigned char>(input.text[input.position++])
            : EOF;
  if (c != EOF)
    ++input.consumed;
  return c;
}
void scan_unget(ScanInput &input, int c) {
  if (c == EOF)
    return;
  if (input.file)
    (void)ungetc(c, input.file);
  else if (input.position)
    --input.position;
  if (input.consumed)
    --input.consumed;
}
bool scanset_contains(const bool set[256], int c, bool invert) {
  if (c < 0 || c > 255)
    return false;
  return invert ? !set[static_cast<unsigned char>(c)]
                : set[static_cast<unsigned char>(c)];
}
int scan_format(ScanInput &input, const char *format, va_list args) {
  if (!format)
    return 0;
  int assigned = 0;
  bool input_failure = false;
  while (*format) {
    if (is_space_char(static_cast<unsigned char>(*format))) {
      while (is_space_char(static_cast<unsigned char>(*format)))
        ++format;
      int c;
      do {
        c = scan_get(input);
      } while (is_space_char(c));
      scan_unget(input, c);
      if (c == EOF)
        input_failure = true;
      continue;
    }
    if (*format != '%') {
      const int c = scan_get(input);
      if (c == EOF) {
        input_failure = true;
        break;
      }
      if (c != static_cast<unsigned char>(*format)) {
        scan_unget(input, c);
        break;
      }
      ++format;
      continue;
    }
    ++format;
    if (*format == '%') {
      const int c = scan_get(input);
      if (c == EOF) {
        input_failure = true;
        break;
      }
      if (c != '%') {
        scan_unget(input, c);
        break;
      }
      ++format;
      continue;
    }
    bool suppress = false;
    if (*format == '*') {
      suppress = true;
      ++format;
    }
    int width = 0;
    while (*format >= '0' && *format <= '9') {
      if (width < 100000)
        width = width * 10 + (*format - '0');
      ++format;
    }
    if (!width)
      width = INT_MAX;
    LengthMod length = LengthMod::NONE;
    if (*format == 'h') {
      ++format;
      length = (*format == 'h') ? (++format, LengthMod::HH) : LengthMod::H;
    } else if (*format == 'l') {
      ++format;
      length = (*format == 'l') ? (++format, LengthMod::LL) : LengthMod::L;
    } else if (*format == 'z') {
      ++format;
      length = LengthMod::Z;
    } else if (*format == 't') {
      ++format;
      length = LengthMod::T;
    } else if (*format == 'j') {
      ++format;
      length = LengthMod::J;
    }
    const char conversion = *format ? *format++ : '\0';
    if (conversion != 'c' && conversion != '[' && conversion != 'n') {
      int c;
      do {
        c = scan_get(input);
      } while (is_space_char(c));
      scan_unget(input, c);
      if (c == EOF) {
        input_failure = true;
        break;
      }
    }
    if (conversion == 'n') {
      if (!suppress) {
        if (length == LengthMod::L)
          *va_arg(args, long *) = static_cast<long>(input.consumed);
        else if (length == LengthMod::LL)
          *va_arg(args, long long *) = static_cast<long long>(input.consumed);
        else if (length == LengthMod::Z)
          *va_arg(args, size_t *) = input.consumed;
        else
          *va_arg(args, int *) = static_cast<int>(input.consumed);
      }
      continue;
    }
    if (conversion == 'c') {
      const int count = width == INT_MAX ? 1 : width;
      char *dest = suppress ? nullptr : va_arg(args, char *);
      int i = 0;
      for (; i < count; ++i) {
        const int c = scan_get(input);
        if (c == EOF)
          break;
        if (dest)
          dest[i] = static_cast<char>(c);
      }
      if (i != count) {
        input_failure = true;
        break;
      }
      if (!suppress)
        ++assigned;
      continue;
    }
    if (conversion == 's' || conversion == '[') {
      bool set[256] = {};
      bool invert = false;
      if (conversion == '[') {
        if (*format == '^') {
          invert = true;
          ++format;
        }
        bool first = true;
        int previous = -1;
        if (*format == ']') {
          set[']'] = true;
          ++format;
          first = false;
        }
        while (*format && (*format != ']' || first)) {
          unsigned char current = static_cast<unsigned char>(*format++);
          first = false;
          if (current == '-' && previous >= 0 && *format && *format != ']') {
            unsigned char end = static_cast<unsigned char>(*format++);
            if (end >= previous)
              for (unsigned v = previous; v <= end; ++v)
                set[v] = true;
            previous = end;
          } else {
            set[current] = true;
            previous = current;
          }
        }
        if (*format == ']')
          ++format;
      }
      char *dest = suppress ? nullptr : va_arg(args, char *);
      int count = 0;
      bool reached_eof = false;
      while (count < width) {
        const int c = scan_get(input);
        if (c == EOF)
          reached_eof = true;
        const bool match = conversion == 's' ? (c != EOF && !is_space_char(c))
                                             : scanset_contains(set, c, invert);
        if (!match) {
          scan_unget(input, c);
          break;
        }
        if (dest)
          dest[count] = static_cast<char>(c);
        ++count;
      }
      if (!count) {
        if (reached_eof)
          input_failure = true;
        break;
      }
      if (dest)
        dest[count] = '\0';
      if (!suppress)
        ++assigned;
      continue;
    }
    if (conversion == 'd' || conversion == 'i' || conversion == 'u' ||
        conversion == 'o' || conversion == 'x' || conversion == 'X' ||
        conversion == 'p') {
      bool negative = false;
      int c = scan_get(input);
      int sign_char = 0;
      if (c == '+' || c == '-') {
        sign_char = c;
        negative = c == '-';
        --width;
        c = scan_get(input);
      }
      if (c == EOF) {
        input_failure = true;
        break;
      }
      unsigned base =
          (conversion == 'o')
              ? 8u
              : ((conversion == 'x' || conversion == 'X' || conversion == 'p')
                     ? 16u
                     : 10u);
      const bool auto_base = conversion == 'i';
      if (auto_base)
        base = 10;
      if (c < '0' || c > '9') {
        scan_unget(input, c);
        if (sign_char)
          scan_unget(input, sign_char);
        break;
      }
      unsigned long long value = static_cast<unsigned>(c - '0');
      int digits = 1;
      --width;
      if (c == '0' && width > 0 && (auto_base || base == 16)) {
        const int prefix = scan_get(input);
        if (prefix == 'x' || prefix == 'X') {
          const int first_hex = scan_get(input);
          const bool valid_hex = (first_hex >= '0' && first_hex <= '9') ||
                                 (first_hex >= 'a' && first_hex <= 'f') ||
                                 (first_hex >= 'A' && first_hex <= 'F');
          if (valid_hex) {
            base = 16;
            value = static_cast<unsigned>(
                first_hex <= '9'
                    ? first_hex - '0'
                    : (first_hex <= 'F' ? (first_hex | 32) - 'a' + 10 : 0));
            digits = 1;
            width -= 2;
          } else {
            scan_unget(input, first_hex);
            scan_unget(input, prefix);
          }
        } else {
          if (auto_base)
            base = 8;
          scan_unget(input, prefix);
        }
      } else if (auto_base && c == '0') {
        base = 8;
      }
      while (width > 0) {
        const int digit_char = scan_get(input);
        if (digit_char == EOF) {
          input_failure = true;
          break;
        }
        unsigned digit;
        if (digit_char >= '0' && digit_char <= '9')
          digit = static_cast<unsigned>(digit_char - '0');
        else if (digit_char >= 'a' && digit_char <= 'f')
          digit = static_cast<unsigned>(digit_char - 'a' + 10);
        else if (digit_char >= 'A' && digit_char <= 'F')
          digit = static_cast<unsigned>(digit_char - 'A' + 10);
        else {
          scan_unget(input, digit_char);
          break;
        }
        if (digit >= base) {
          scan_unget(input, digit_char);
          break;
        }
        value = value * base + digit;
        ++digits;
        --width;
      }
      if (!digits) {
        if (c == '+' || c == '-')
          scan_unget(input, c);
        break;
      }
      if (!suppress) {
        if (conversion == 'p')
          *va_arg(args, void **) = reinterpret_cast<void *>(
              static_cast<uintptr_t>(negative ? 0ull - value : value));
        else if (conversion == 'd' || conversion == 'i') {
          const long long signed_value =
              negative ? static_cast<long long>(0ull - value)
                       : static_cast<long long>(value);
          if (length == LengthMod::LL || length == LengthMod::J)
            *va_arg(args, long long *) = signed_value;
          else if (length == LengthMod::L)
            *va_arg(args, long *) = static_cast<long>(signed_value);
          else if (length == LengthMod::Z || length == LengthMod::T)
            *va_arg(args, ptrdiff_t *) = static_cast<ptrdiff_t>(signed_value);
          else if (length == LengthMod::H)
            *va_arg(args, short *) = static_cast<short>(signed_value);
          else if (length == LengthMod::HH)
            *va_arg(args, signed char *) =
                static_cast<signed char>(signed_value);
          else
            *va_arg(args, int *) = static_cast<int>(signed_value);
        } else {
          if (length == LengthMod::LL || length == LengthMod::J)
            *va_arg(args, unsigned long long *) = value;
          else if (length == LengthMod::L)
            *va_arg(args, unsigned long *) = static_cast<unsigned long>(value);
          else if (length == LengthMod::Z)
            *va_arg(args, size_t *) = static_cast<size_t>(value);
          else if (length == LengthMod::H)
            *va_arg(args, unsigned short *) =
                static_cast<unsigned short>(value);
          else if (length == LengthMod::HH)
            *va_arg(args, unsigned char *) = static_cast<unsigned char>(value);
          else
            *va_arg(args, unsigned int *) = static_cast<unsigned int>(value);
        }
        ++assigned;
      }
      continue;
    }
    if (!conversion)
      break;
    break;
  }
  return assigned ? assigned : (input_failure ? EOF : 0);
}
} // namespace

extern "C" {
int vsnprintf(char *buffer, size_t size, const char *format, va_list args) {
  va_list copy;
  va_copy(copy, args);
  const int result = format_to_buffer(buffer, size, format, copy);
  va_end(copy);
  return result;
}
int snprintf(char *buffer, size_t size, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int result = vsnprintf(buffer, size, format, args);
  va_end(args);
  return result;
}
int vsprintf(char *buffer, const char *format, va_list args) {
  return vsnprintf(buffer, static_cast<size_t>(-1), format, args);
}
int sprintf(char *buffer, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int result = vsprintf(buffer, format, args);
  va_end(args);
  return result;
}

int vfprintf(FILE *stream, const char *format, va_list args) {
  if (!stream) {
    errno_val = 9;
    return -1;
  }
  va_list count_args;
  va_copy(count_args, args);
  const int length = vsnprintf(nullptr, 0, format, count_args);
  va_end(count_args);
  if (length < 0)
    return -1;
  char local[STACK_FORMAT_SIZE];
  char *buffer = local;
  if (static_cast<size_t>(length) + 1 > sizeof(local)) {
    buffer = static_cast<char *>(malloc(static_cast<size_t>(length) + 1));
    if (!buffer) {
      errno_val = 12;
      return -1;
    }
  }
  va_list render_args;
  va_copy(render_args, args);
  const int rendered =
      vsnprintf(buffer, static_cast<size_t>(length) + 1, format, render_args);
  va_end(render_args);
  int result = rendered < 0 ? -1
                            : stream_write(stream, buffer,
                                           static_cast<size_t>(rendered));
  if (buffer != local)
    free(buffer);
  return result < 0 ? -1 : rendered;
}
int fprintf(FILE *stream, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int result = vfprintf(stream, format, args);
  va_end(args);
  return result;
}
int vprintf(const char *format, va_list args) {
  return vfprintf(stdout, format, args);
}
int printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int result = vfprintf(stdout, format, args);
  va_end(args);
  return result;
}
int puts(const char *text) {
  if (fputs(text ? text : "(null)", stdout) == EOF ||
      fputc('\n', stdout) == EOF)
    return EOF;
  return 0;
}
int fputs(const char *text, FILE *stream) {
  if (!text)
    text = "(null)";
  const size_t length = strlen(text);
  return stream_write(stream, text, length) == static_cast<int>(length) ? 0
                                                                        : EOF;
}
int fputc(int character, FILE *stream) {
  const unsigned char ch = static_cast<unsigned char>(character);
  return stream_write(stream, &ch, 1) == 1 ? ch : EOF;
}
int putc(int character, FILE *stream) { return fputc(character, stream); }
int putchar(int character) { return fputc(character, stdout); }
int fgetc(FILE *stream) { return stream_get(stream); }
int getc(FILE *stream) { return fgetc(stream); }
int getchar(void) { return fgetc(stdin); }
int ungetc(int character, FILE *stream) {
  if (!stream || character == EOF)
    return EOF;
  if (is_stdin_stream(stream) && Terminal::push_input_char(character))
    return static_cast<unsigned char>(character);
  if (stream->unget_count >= sizeof(stream->unget_buffer))
    return EOF;
  stream->unget_buffer[stream->unget_count++] =
      static_cast<unsigned char>(character);
  stream->eof = 0;
  return static_cast<unsigned char>(character);
}

char *fgets(char *buffer, int size, FILE *stream) {
  if (!buffer || !stream || size <= 0) {
    errno_val = 22;
    return nullptr;
  }
  if (size == 1) {
    buffer[0] = '\0';
    return buffer;
  }
  int count = 0;
  while (count < size - 1) {
    const int ch = fgetc(stream);
    if (ch == EOF)
      break;
    buffer[count++] = static_cast<char>(ch);
    if (ch == '\n')
      break;
  }
  if (!count)
    return nullptr;
  buffer[count] = '\0';
  return buffer;
}

FILE *fopen(const char *filename, const char *mode) {
  if (!filename || !mode) {
    errno_val = 22;
    return nullptr;
  }
  void *handle = Kernel::FS::VFS::vfs_open(filename, mode);
  if (!handle) {
    errno_val = 2;
    return nullptr;
  }
  FILE *file = static_cast<FILE *>(malloc(sizeof(FILE)));
  if (!file) {
    Kernel::FS::VFS::vfs_close(handle);
    errno_val = 12;
    return nullptr;
  }
  *file = FILE{};
  file->stream = handle;
  file->descriptor = __atomic_fetch_add(&next_descriptor, 1, __ATOMIC_RELAXED);
  return file;
}
int fclose(FILE *stream) {
  if (!stream || is_stdout_stream(stream) || is_stderr_stream(stream) ||
      is_stdin_stream(stream)) {
    errno_val = 9;
    return EOF;
  }
  const int result = Kernel::FS::VFS::vfs_close(stream->stream);
  free(stream);
  return result == 0 ? 0 : EOF;
}
size_t fread(void *buffer, size_t size, size_t count, FILE *stream) {
  if (!stream || (!buffer && size && count) || !size || !count)
    return 0;
  if (count > static_cast<size_t>(-1) / size) {
    stream->error = 1;
    errno_val = 75;
    return 0;
  }
  const size_t total = size * count;
  unsigned char *output = static_cast<unsigned char *>(buffer);
  size_t bytes = 0;
  while (stream->unget_count && bytes < total)
    output[bytes++] = stream->unget_buffer[--stream->unget_count];
  if (is_stdin_stream(stream)) {
    while (bytes < total) {
      const int ch = fgetc(stream);
      if (ch == EOF)
        break;
      output[bytes++] = static_cast<unsigned char>(ch);
    }
  } else if (is_file_stream(stream) && bytes < total) {
    bytes += Kernel::FS::VFS::vfs_read(output + bytes, 1, total - bytes,
                                       stream->stream);
  } else if (bytes < total) {
    stream->error = 1;
    errno_val = 9;
  }
  if (bytes < total && is_file_stream(stream) &&
      Kernel::FS::VFS::vfs_tell(stream->stream) >=
          static_cast<long>(Kernel::FS::VFS::vfs_size(stream->stream)))
    stream->eof = 1;
  return bytes / size;
}
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream) {
  if (!stream || (!buffer && size && count) || !size || !count)
    return 0;
  if (count > static_cast<size_t>(-1) / size) {
    stream->error = 1;
    errno_val = 75;
    return 0;
  }
  const size_t total = size * count;
  const int written = stream_write(stream, buffer, total);
  return written < 0 ? 0 : static_cast<size_t>(written) / size;
}
int fflush(FILE *stream) {
  if (!stream) {
    Terminal::flush();
    return !Kernel::FS::VFS::is_mounted() || Kernel::FS::VFS::vfs_sync() ? 0
                                                                         : EOF;
  }
  if (is_stdout_stream(stream) || is_stderr_stream(stream)) {
    Terminal::flush();
    return 0;
  }
  if (is_file_stream(stream))
    return Kernel::FS::VFS::vfs_sync() ? 0 : EOF;
  errno_val = 9;
  return EOF;
}
int fseek(FILE *stream, long offset, int whence) {
  if (!is_file_stream(stream) ||
      (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)) {
    errno_val = 9;
    return EOF;
  }
  stream->unget_count = 0;
  stream->eof = 0;
  const int result = Kernel::FS::VFS::vfs_seek(stream->stream, offset, whence);
  if (result != 0)
    stream->error = 1;
  return result;
}
long ftell(FILE *stream) {
  if (!is_file_stream(stream)) {
    errno_val = 9;
    return -1;
  }
  const long position = Kernel::FS::VFS::vfs_tell(stream->stream);
  return position < 0 ? position : position - stream->unget_count;
}
void rewind(FILE *stream) {
  if (fseek(stream, 0, SEEK_SET) == 0)
    clearerr(stream);
}
int fgetpos(FILE *stream, fpos_t *position) {
  if (!position) {
    errno_val = 22;
    return EOF;
  }
  const long value = ftell(stream);
  if (value < 0)
    return EOF;
  *position = static_cast<fpos_t>(value);
  return 0;
}
int fsetpos(FILE *stream, const fpos_t *position) {
  if (!position) {
    errno_val = 22;
    return EOF;
  }
  return fseek(stream, static_cast<long>(*position), SEEK_SET);
}
int feof(FILE *stream) { return stream ? stream->eof != 0 : 0; }
int ferror(FILE *stream) { return stream ? stream->error != 0 : 1; }
void clearerr(FILE *stream) {
  if (stream)
    stream->eof = stream->error = 0;
}
int fileno(FILE *stream) { return stream ? stream->descriptor : -1; }
int isatty(int descriptor) { return descriptor >= 0 && descriptor <= 2; }
int setvbuf(FILE *stream, char *, int mode, size_t) {
  if (!stream || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)) {
    errno_val = 22;
    return EOF;
  }
  if (mode != _IONBF) {
    errno_val = 38;
    return EOF;
  }
  return 0;
}
void setbuf(FILE *stream, char *) { (void)setvbuf(stream, nullptr, _IONBF, 0); }

int vfscanf(FILE *stream, const char *format, va_list args) {
  if (!stream) {
    errno_val = 9;
    return EOF;
  }
  ScanInput input{stream, nullptr, 0, 0};
  return scan_format(input, format, args);
}
int fscanf(FILE *stream, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int result = vfscanf(stream, format, args);
  va_end(args);
  return result;
}
int scanf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int result = vfscanf(stdin, format, args);
  va_end(args);
  return result;
}
int vscanf(const char *format, va_list args) {
  return vfscanf(stdin, format, args);
}
int vsscanf(const char *text, const char *format, va_list args) {
  if (!text)
    return EOF;
  ScanInput input{nullptr, text, 0, 0};
  return scan_format(input, format, args);
}
int sscanf(const char *text, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int result = vsscanf(text, format, args);
  va_end(args);
  return result;
}
int __isoc99_sscanf(const char *text, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int result = vsscanf(text, format, args);
  va_end(args);
  return result;
}
void perror(const char *prefix) {
  if (prefix && *prefix)
    fprintf(stderr, "%s: error %d\n", prefix, errno_val);
  else
    fprintf(stderr, "error %d\n", errno_val);
}

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

extern "C" void kprintf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  (void)vfprintf(stdout, format, args);
  va_end(args);
}
