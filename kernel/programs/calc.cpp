#include <arch/x86_64/string.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/uicommon.h>
#include <kernel/lib/appcommon.h>
#include <kernel/scheduler.h>
#include <stddef.h>
#include <stdint.h>

namespace Kernel::Apps::Calc {
using Kernel::Drivers::Keyboard;
using namespace Kernel::AppCommon;

static const double PI_VAL = 3.141592653589793;

inline double fmod_custom(double a, double b) {
#if defined(__x86_64__) || defined(_M_X64)
  return a - __builtin_trunc(a / b) * b;
#elif defined(__ARM_ARCH) || defined(__aarch64__)
  return a - __builtin_trunc(a / b) * b;
#else
  double quotient = a / b;
  double magic = (quotient < 0.0) ? -4503599627370496.0 : 4503599627370496.0;
  double trc = (quotient + magic) - magic;
  if ((quotient > 0.0 && trc > quotient) ||
      (quotient < 0.0 && trc < quotient)) {
    trc += (quotient < 0.0) ? 1.0 : -1.0;
  }
  return a - trc * b;
#endif
}

static double sin_approx(double x) {
  x = fmod_custom(x + PI_VAL, 2.0 * PI_VAL) - PI_VAL;
  double x2 = x * x;
  double term = x;
  double sum = term;
  double sign = -1.0;
  for (int i = 3; i <= 15; i += 2) {
    term *= x2 / (static_cast<double>(i - 1) * i);
    sum += sign * term;
    sign = -sign;
  }
  return sum;
}

static double cos_approx(double x) {
  x = fmod_custom(x + PI_VAL, 2.0 * PI_VAL) - PI_VAL;
  double x2 = x * x;
  double term = 1.0;
  double sum = term;
  double sign = -1.0;
  for (int i = 2; i <= 14; i += 2) {
    term *= x2 / (static_cast<double>(i - 1) * i);
    sum += sign * term;
    sign = -sign;
  }
  return sum;
}

static double tan_approx(double x) {
  double c = cos_approx(x);
  if (c > -1e-12 && c < 1e-12)
    return 1e12;
  return sin_approx(x) / c;
}

static double sqrt_approx(double x) {
  if (x < 0)
    return -1.0;
  if (x == 0)
    return 0.0;
  double guess = x / 2.0;
  if (guess == 0)
    guess = 1.0;
  for (int i = 0; i < 10; ++i) {
    guess = 0.5 * (guess + x / guess);
  }
  return guess;
}

static double ln_approx(double x) {
  if (x <= 0.0)
    return -1e12;
  int k = 0;
  while (x < 0.7071067811865476) {
    x *= 2.0;
    k--;
  }
  while (x >= 1.4142135623730951) {
    x *= 0.5;
    k++;
  }

  double y = (x - 1.0) / (x + 1.0);
  double y2 = y * y;
  double term = y;
  double sum = term;
  double denom = 3.0;
  for (int i = 0; i < 15; ++i) {
    term *= y2;
    sum += term / denom;
    denom += 2.0;
  }
  const double LN2 = 0.69314718055994530942;
  return 2.0 * sum + static_cast<double>(k) * LN2;
}

static double exp_approx(double x) {
  if (x > 709.0)
    return 1.0e12;
  if (x < -709.0)
    return 0.0;

  const double INV_LN2 = 1.4426950408889634074;
  const double LN2 = 0.69314718055994530942;

  long long k = static_cast<long long>(x * INV_LN2);
  if (x < 0.0 && (x * INV_LN2) != static_cast<double>(k)) {
    k--;
  }
  double r = x - static_cast<double>(k) * LN2;

  double sum = 1.0;
  double term = 1.0;
  for (int i = 1; i <= 20; ++i) {
    term *= r / static_cast<double>(i);
    sum += term;
  }

  // Compute 2^k scaling factor
  double scale = 1.0;
  long long abs_k = k < 0 ? -k : k;
  double base_2 = 2.0;
  while (abs_k > 0) {
    if (abs_k & 1)
      scale *= base_2;
    base_2 *= base_2;
    abs_k >>= 1;
  }
  if (k < 0)
    scale = 1.0 / scale;

  return sum * scale;
}

static double log10_approx(double x) { return ln_approx(x) / ln_approx(10.0); }

static double logn_approx(double x, double n) {
  double l1 = ln_approx(n);
  if (l1 == 0.0)
    return 0.0;
  return ln_approx(x) / l1;
}

static double power(double base, double exp) {
  if (exp == 0.0)
    return 1.0;
  if (base == 0.0)
    return 0.0;
  if (base > 0.0) {
    return exp_approx(exp * ln_approx(base));
  } else {
    int iexp = static_cast<int>(exp);
    if (static_cast<double>(iexp) == exp) {
      double res = 1.0;
      double b = base;
      int e = iexp < 0 ? -iexp : iexp;
      while (e > 0) {
        if (e & 1)
          res *= b;
        b *= b;
        e >>= 1;
      }
      return iexp < 0 ? 1.0 / res : res;
    }
    return 0.0;
  }
}

// -----------------------------------------------------------------------------
// Number Formatting
// -----------------------------------------------------------------------------

static void format_number(double value, char *out) {
  if (value != value || value > 1.0e12 || value < -1.0e12) {
    strcpy(out, "Error");
    return;
  }
  if (value == 0.0) {
    strcpy(out, "0");
    return;
  }

  size_t pos = 0;
  if (value < 0.0) {
    out[pos++] = '-';
    value = -value;
  }

  uint64_t whole = static_cast<uint64_t>(value);
  double fraction = value - static_cast<double>(whole);

  char digits[24];
  size_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + whole % 10);
    whole /= 10;
  } while (whole && n < sizeof(digits));

  while (n)
    out[pos++] = digits[--n];

  uint64_t decimal = static_cast<uint64_t>(fraction * 10000000.0 + 0.5);
  if (decimal >= 10000000ULL) {
    decimal = 0;
    size_t i = pos;
    while (i && out[i - 1] == '9')
      out[--i] = '0';
    if (i && out[i - 1] >= '0' && out[i - 1] <= '8')
      ++out[i - 1];
    else {
      for (size_t j = pos; j > 0; --j)
        out[j] = out[j - 1];
      out[0] = '1';
      ++pos;
    }
  }

  if (decimal) {
    out[pos++] = '.';
    uint64_t divisor = 1000000;
    for (int i = 0; i < 7; ++i) {
      out[pos++] = static_cast<char>('0' + (decimal / divisor) % 10);
      divisor /= 10;
    }
    while (pos && out[pos - 1] == '0')
      --pos;
    if (pos && out[pos - 1] == '.')
      --pos;
  }
  out[pos] = '\0';
}

// -----------------------------------------------------------------------------
// Recursive Descent Expression Parser (PEMDAS / BODMAS)
// -----------------------------------------------------------------------------

class Parser {
  const char *p;
  bool error_flag;

public:
  Parser(const char *str) : p(str), error_flag(false) {}
  bool has_error() const { return error_flag; }

  double parse() {
    skip_whitespace();
    double res = parse_expr();
    skip_whitespace();
    if (*p != '\0')
      error_flag = true;
    return res;
  }

private:
  void skip_whitespace() {
    while (*p == ' ' || *p == '\t')
      p++;
  }

  double parse_expr() {
    double val = parse_term();
    if (error_flag)
      return 0.0;
    skip_whitespace();
    while (*p == '+' || *p == '-') {
      char op = *p++;
      double next = parse_term();
      if (error_flag)
        return 0.0;
      if (op == '+')
        val += next;
      else
        val -= next;
      skip_whitespace();
    }
    return val;
  }

  double parse_term() {
    double val = parse_factor();
    if (error_flag)
      return 0.0;
    skip_whitespace();
    while (*p == '*' || *p == '/') {
      char op = *p++;
      double next = parse_factor();
      if (error_flag)
        return 0.0;
      if (op == '*')
        val *= next;
      else {
        if (next == 0.0) {
          error_flag = true;
          return 0.0;
        }
        val /= next;
      }
      skip_whitespace();
    }
    return val;
  }

  double parse_factor() {
    double val = parse_unary();
    if (error_flag)
      return 0.0;
    skip_whitespace();
    if (*p == '^') {
      p++;
      double exp = parse_factor();
      if (error_flag)
        return 0.0;
      val = power(val, exp);
    }
    return val;
  }

  double parse_unary() {
    skip_whitespace();
    if (*p == '-') {
      p++;
      return -parse_primary();
    } else if (*p == '+') {
      p++;
      return parse_primary();
    }
    return parse_primary();
  }

  double parse_primary() {
    skip_whitespace();
    if (*p == '\0') {
      error_flag = true;
      return 0.0;
    }

    if (*p == '(') {
      p++;
      double val = parse_expr();
      skip_whitespace();
      if (*p == ')')
        p++;
      else
        error_flag = true;
      return val;
    }

    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
      char ident[32];
      int idx = 0;
      while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
             (*p >= '0' && *p <= '9')) {
        if (idx < 31)
          ident[idx++] = *p;
        p++;
      }
      ident[idx] = '\0';

      for (int i = 0; ident[i]; i++) {
        if (ident[i] >= 'A' && ident[i] <= 'Z')
          ident[i] += 32;
      }

      if (strcmp(ident, "pi") == 0) {
        return PI_VAL;
      }

      if (strcmp(ident, "sin") == 0 || strcmp(ident, "cos") == 0 ||
          strcmp(ident, "tan") == 0 || strcmp(ident, "sqrt") == 0 ||
          strcmp(ident, "log10") == 0) {

        skip_whitespace();
        if (*p != '(') {
          error_flag = true;
          return 0.0;
        }
        p++;
        double arg = parse_expr();
        skip_whitespace();
        if (*p != ')') {
          error_flag = true;
          return 0.0;
        }
        p++;

        if (error_flag)
          return 0.0;
        if (strcmp(ident, "sin") == 0)
          return sin_approx(arg);
        if (strcmp(ident, "cos") == 0)
          return cos_approx(arg);
        if (strcmp(ident, "tan") == 0)
          return tan_approx(arg);
        if (strcmp(ident, "sqrt") == 0) {
          double s = sqrt_approx(arg);
          if (s < 0) {
            error_flag = true;
            return 0.0;
          }
          return s;
        }
        if (strcmp(ident, "log10") == 0) {
          if (arg <= 0) {
            error_flag = true;
            return 0.0;
          }
          return log10_approx(arg);
        }
      }

      if (strcmp(ident, "logn") == 0 || strcmp(ident, "root") == 0) {
        skip_whitespace();
        if (*p != '(') {
          error_flag = true;
          return 0.0;
        }
        p++;
        double arg1 = parse_expr();
        skip_whitespace();
        if (*p != ',') {
          error_flag = true;
          return 0.0;
        }
        p++;
        double arg2 = parse_expr();
        skip_whitespace();
        if (*p != ')') {
          error_flag = true;
          return 0.0;
        }
        p++;

        if (error_flag)
          return 0.0;
        if (strcmp(ident, "logn") == 0) {
          if (arg1 <= 0 || arg2 <= 0 || arg2 == 1.0) {
            error_flag = true;
            return 0.0;
          }
          return logn_approx(arg1, arg2);
        }
        if (strcmp(ident, "root") == 0) {
          if (arg2 == 0.0) {
            error_flag = true;
            return 0.0;
          }
          return power(arg1, 1.0 / arg2);
        }
      }

      error_flag = true;
      return 0.0;
    }

    if ((*p >= '0' && *p <= '9') || *p == '.') {
      double val = 0.0;
      double scale = 0.1;
      bool fractional = false;

      while ((*p >= '0' && *p <= '9') || *p == '.') {
        if (*p == '.') {
          if (fractional) {
            error_flag = true;
            return 0.0;
          }
          fractional = true;
        } else {
          if (!fractional) {
            val = val * 10.0 + (*p - '0');
          } else {
            val += (*p - '0') * scale;
            scale *= 0.1;
          }
        }
        p++;
      }
      return val;
    }

    error_flag = true;
    return 0.0;
  }
};

// -----------------------------------------------------------------------------
// Syntax Highlighting Drawer
// -----------------------------------------------------------------------------

static void
draw_highlighted_expression(Kernel::Graphics::VirtualFramebuffer &vfb,
                            int start_x, int y, const char *expr) {
  int cx = start_x;
  const char *p = expr;
  while (*p != '\0') {
    if (*p == ' ' || *p == '\t') {
      char space[2] = {*p, '\0'};
      Kernel::UICommon::text(vfb, cx, y, space, 0xFFCDD6F4);
      cx += Kernel::UICommon::text_width(space);
      p++;
      continue;
    }

    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
      char word[32];
      int idx = 0;
      while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
             (*p >= '0' && *p <= '9')) {
        if (idx < 31)
          word[idx++] = *p;
        p++;
      }
      word[idx] = '\0';
      Kernel::UICommon::text(vfb, cx, y, word, 0xFFF9E2AF);
      cx += Kernel::UICommon::text_width(word);
      continue;
    }

    if ((*p >= '0' && *p <= '9') || *p == '.') {
      char number[32];
      int idx = 0;
      while ((*p >= '0' && *p <= '9') || *p == '.') {
        if (idx + 1 < static_cast<int>(sizeof(number)))
          number[idx++] = *p;
        p++;
      }
      number[idx] = '\0';
      Kernel::UICommon::text(vfb, cx, y, number, 0xFFFAB387);
      cx += Kernel::UICommon::text_width(number);
      continue;
    }

    if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '^' ||
        *p == ',') {
      char token[2] = {*p, '\0'};
      Kernel::UICommon::text(vfb, cx, y, token, 0xFF89B4FA);
      cx += Kernel::UICommon::text_width(token);
      p++;
      continue;
    }

    if (*p == '(' || *p == ')') {
      char token[2] = {*p, '\0'};
      Kernel::UICommon::text(vfb, cx, y, token, 0xFF94E2D5);
      cx += Kernel::UICommon::text_width(token);
      p++;
      continue;
    }

    char token[2] = {*p, '\0'};
    Kernel::UICommon::text(vfb, cx, y, token, 0xFFCDD6F4);
    cx += Kernel::UICommon::text_width(token);
    p++;
  }
}

// -----------------------------------------------------------------------------
// App Entry Point
// -----------------------------------------------------------------------------

void run() {
  Kernel::Graphics::VirtualFramebuffer vfb;
  if (!vfb.open_window(600, 460, 300, 200))
    return;
  const int32_t w = static_cast<int32_t>(vfb.width());
  const int32_t h = static_cast<int32_t>(vfb.height());

  char expression[96] = "";
  char display[64] = "0";
  bool evaluated = false;
  bool error = false;
  bool running = true;

  while (running && !Kernel::Scheduler::should_stop()) {
    while (Keyboard::available()) {
      char c = Keyboard::getchar();

      if (c == 27) {
        running = false;
        break;
      }

      if (c == '`') {
        expression[0] = '\0';
        strcpy(display, "0");
        evaluated = false;
        error = false;
        continue;
      }

      if (c == '\b' || c == 127) {
        size_t len = strlen(expression);
        if (len > 0) {
          expression[len - 1] = '\0';
          if (strlen(expression) > 0) {
            strcpy(display, expression);
          } else {
            strcpy(display, "0");
          }
          error = false;
          evaluated = false;
        }
        continue;
      }

      if (c == '=' || c == '\n' || c == '\r') {
        if (strlen(expression) == 0)
          continue;
        Parser parser(expression);
        double res = parser.parse();
        if (!parser.has_error()) {
          format_number(res, display);
          strcpy(expression, display);
          evaluated = true;
          error = false;
        } else {
          strcpy(display, "Error");
          error = true;
        }
        continue;
      }

      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' ||
          c == '*' || c == '/' || c == '(' || c == ')' || c == '^' ||
          c == ',') {

        if (evaluated) {
          if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z')) {
            expression[0] = '\0';
          }
          evaluated = false;
        }

        if (error) {
          expression[0] = '\0';
          error = false;
        }

        size_t len = strlen(expression);
        if (len < sizeof(expression) - 1) {
          expression[len] = c;
          expression[len + 1] = '\0';
          strcpy(display, expression);
        }
      }
    }

    if (!running)
      break;

    vfb.clear(0xFF1E1E2E);

    vfb.fill_rect(20, 20, w - 40, 60, 0xFF313244);
    if (strlen(expression) > 0) {
      draw_highlighted_expression(vfb, 32, 42, expression);
    } else {
      Kernel::UICommon::text(vfb, 32, 42, display, error ? 0xFFF38BA8 : 0xFFCDD6F4);
    }

    // Minimalist help guide
    Kernel::UICommon::text(vfb, 20, 100, "Functions: sin, cos, tan, sqrt, root, log10, logn, pi",
         0xFF6C7086);
    Kernel::UICommon::text(vfb, 20, 118, "Exponents: ^-1, ^-2 | Enter: = | `: Clear | ESC: Quit",
         0xFF6C7086);

    vfb.flush();
    delay(10);
  }
}

} // namespace Kernel::Apps::Calc