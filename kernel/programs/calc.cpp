#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/appcommon.h>
#include <kernel/scheduler.h>
#include <arch/x86_64/string.h>
#include <stdint.h>
#include <stddef.h>

namespace Kernel::Apps::Calc {
using Kernel::Drivers::Framebuffer::width;
using Kernel::Drivers::Framebuffer::height;
using Kernel::Drivers::Keyboard;
using namespace Kernel::AppCommon;

// -----------------------------------------------------------------------------
// Mathematical Constants & Precise Approximations
// -----------------------------------------------------------------------------

static const double PI_VAL = 3.141592653589793;

static double fmod_custom(double a, double b) {
    return a - static_cast<int>(a / b) * b;
}

static double sin_approx(double x) {
    x = fmod_custom(x + PI_VAL, 2.0 * PI_VAL) - PI_VAL;
    double x2 = x * x;
    return x * (1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0))));
}

static double cos_approx(double x) {
    x = fmod_custom(x + PI_VAL, 2.0 * PI_VAL) - PI_VAL;
    double x2 = x * x;
    return 1.0 - x2 / 2.0 * (1.0 - x2 / 12.0 * (1.0 - x2 / 30.0 * (1.0 - x2 / 56.0)));
}

static double tan_approx(double x) {
    double c = cos_approx(x);
    if (c == 0.0) return 1e12;
    return sin_approx(x) / c;
}

static double sqrt_approx(double x) {
    if (x < 0) return -1.0;
    if (x == 0) return 0.0;
    double guess = x / 2.0;
    if (guess == 0) guess = 1.0;
    for (int i = 0; i < 10; ++i) {
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

static double ln_approx(double x) {
    if (x <= 0) return -1e12;
    double y = (x - 1.0) / (x + 1.0);
    double y2 = y * y;
    double term = y;
    double sum = term;
    for (int i = 3; i <= 15; i += 2) {
        term *= y2;
        sum += term / i;
    }
    return 2.0 * sum;
}

static double log10_approx(double x) {
    return ln_approx(x) / ln_approx(10.0);
}

static double logn_approx(double x, double n) {
    double l1 = ln_approx(n);
    if (l1 == 0.0) return 0.0;
    return ln_approx(x) / l1;
}

static double power(double base, double exp) {
    if (exp == 0.0) return 1.0;
    if (base == 0.0) return 0.0;
    if (base > 0.0) {
        double z = exp * ln_approx(base);
        double sum = 1.0, term = 1.0;
        for (int i = 1; i <= 20; ++i) {
            term *= z / i;
            sum += term;
        }
        return sum;
    } else {
        int iexp = static_cast<int>(exp);
        if (static_cast<double>(iexp) == exp) {
            double res = 1.0;
            double b = base;
            int e = iexp < 0 ? -iexp : iexp;
            while (e > 0) {
                if (e & 1) res *= b;
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

static void format_number(double value, char* out) {
    if (value != value || value > 1.0e12 || value < -1.0e12) {
        strcpy(out, "Error");
        return;
    }
    if (value == 0.0) { strcpy(out, "0"); return; }

    size_t pos = 0;
    if (value < 0.0) { out[pos++] = '-'; value = -value; }

    uint64_t whole = static_cast<uint64_t>(value);
    double fraction = value - static_cast<double>(whole);

    char digits[24];
    size_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + whole % 10);
        whole /= 10;
    } while (whole && n < sizeof(digits));

    while (n) out[pos++] = digits[--n];

    uint64_t decimal = static_cast<uint64_t>(fraction * 10000000.0 + 0.5);
    if (decimal >= 10000000ULL) {
        decimal = 0;
        size_t i = pos;
        while (i && out[i - 1] == '9') out[--i] = '0';
        if (i && out[i - 1] >= '0' && out[i - 1] <= '8') ++out[i - 1];
        else {
            for (size_t j = pos; j > 0; --j) out[j] = out[j - 1];
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
        while (pos && out[pos - 1] == '0') --pos;
        if (pos && out[pos - 1] == '.') --pos;
    }
    out[pos] = '\0';
}

// -----------------------------------------------------------------------------
// Recursive Descent Expression Parser (PEMDAS / BODMAS)
// -----------------------------------------------------------------------------

class Parser {
    const char* p;
    bool error_flag;
public:
    Parser(const char* str) : p(str), error_flag(false) {}
    bool has_error() const { return error_flag; }

    double parse() {
        skip_whitespace();
        double res = parse_expr();
        skip_whitespace();
        if (*p != '\0') error_flag = true;
        return res;
    }

private:
    void skip_whitespace() {
        while (*p == ' ' || *p == '\t') p++;
    }

    double parse_expr() {
        double val = parse_term();
        if (error_flag) return 0.0;
        skip_whitespace();
        while (*p == '+' || *p == '-') {
            char op = *p++;
            double next = parse_term();
            if (error_flag) return 0.0;
            if (op == '+') val += next;
            else val -= next;
            skip_whitespace();
        }
        return val;
    }

    double parse_term() {
        double val = parse_factor();
        if (error_flag) return 0.0;
        skip_whitespace();
        while (*p == '*' || *p == '/') {
            char op = *p++;
            double next = parse_factor();
            if (error_flag) return 0.0;
            if (op == '*') val *= next;
            else {
                if (next == 0.0) { error_flag = true; return 0.0; }
                val /= next;
            }
            skip_whitespace();
        }
        return val;
    }

    double parse_factor() {
        double val = parse_unary();
        if (error_flag) return 0.0;
        skip_whitespace();
        if (*p == '^') {
            p++;
            double exp = parse_factor();
            if (error_flag) return 0.0;
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
        if (*p == '\0') { error_flag = true; return 0.0; }

        if (*p == '(') {
            p++;
            double val = parse_expr();
            skip_whitespace();
            if (*p == ')') p++;
            else error_flag = true;
            return val;
        }

        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            char ident[32];
            int idx = 0;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) {
                if (idx < 31) ident[idx++] = *p;
                p++;
            }
            ident[idx] = '\0';

            for (int i = 0; ident[i]; i++) {
                if (ident[i] >= 'A' && ident[i] <= 'Z') ident[i] += 32;
            }

            if (strcmp(ident, "pi") == 0) {
                return PI_VAL;
            }

            if (strcmp(ident, "sin") == 0 || strcmp(ident, "cos") == 0 ||
                strcmp(ident, "tan") == 0 || strcmp(ident, "sqrt") == 0 ||
                strcmp(ident, "log10") == 0) {

                skip_whitespace();
                if (*p != '(') { error_flag = true; return 0.0; }
                p++;
                double arg = parse_expr();
                skip_whitespace();
                if (*p != ')') { error_flag = true; return 0.0; }
                p++;

                if (error_flag) return 0.0;
                if (strcmp(ident, "sin") == 0) return sin_approx(arg);
                if (strcmp(ident, "cos") == 0) return cos_approx(arg);
                if (strcmp(ident, "tan") == 0) return tan_approx(arg);
                if (strcmp(ident, "sqrt") == 0) {
                    double s = sqrt_approx(arg);
                    if (s < 0) { error_flag = true; return 0.0; }
                    return s;
                }
                if (strcmp(ident, "log10") == 0) {
                    if (arg <= 0) { error_flag = true; return 0.0; }
                    return log10_approx(arg);
                }
            }

            if (strcmp(ident, "logn") == 0 || strcmp(ident, "root") == 0) {
                skip_whitespace();
                if (*p != '(') { error_flag = true; return 0.0; }
                p++;
                double arg1 = parse_expr();
                skip_whitespace();
                if (*p != ',') { error_flag = true; return 0.0; }
                p++;
                double arg2 = parse_expr();
                skip_whitespace();
                if (*p != ')') { error_flag = true; return 0.0; }
                p++;

                if (error_flag) return 0.0;
                if (strcmp(ident, "logn") == 0) {
                    if (arg1 <= 0 || arg2 <= 0 || arg2 == 1.0) { error_flag = true; return 0.0; }
                    return logn_approx(arg1, arg2);
                }
                if (strcmp(ident, "root") == 0) {
                    if (arg2 == 0.0) { error_flag = true; return 0.0; }
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
                    if (fractional) { error_flag = true; return 0.0; }
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

static void draw_highlighted_expression(Kernel::Graphics::VirtualFramebuffer& vfb, int start_x, int y, const char* expr) {
    int cx = start_x;
    const char* p = expr;
    while (*p != '\0') {
        if (*p == ' ' || *p == '\t') {
            cx += 8;
            p++;
            continue;
        }

        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            char word[32];
            int idx = 0;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) {
                if (idx < 31) word[idx++] = *p;
                p++;
            }
            word[idx] = '\0';
            for (int i = 0; i < idx; ++i) {
                vfb.draw_glyph(cx, y, word[i], 0xFFF9E2AF); // Yellow for functions and pi
                cx += 8;
            }
            continue;
        }

        if ((*p >= '0' && *p <= '9') || *p == '.') {
            while ((*p >= '0' && *p <= '9') || *p == '.') {
                vfb.draw_glyph(cx, y, *p, 0xFFFAB387); // Peach for numbers
                cx += 8;
                p++;
            }
            continue;
        }

        if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '^' || *p == ',') {
            vfb.draw_glyph(cx, y, *p, 0xFF89B4FA); // Blue for operators
            cx += 8;
            p++;
            continue;
        }

        if (*p == '(' || *p == ')') {
            vfb.draw_glyph(cx, y, *p, 0xFF94E2D5); // Teal for parentheses
            cx += 8;
            p++;
            continue;
        }

        vfb.draw_glyph(cx, y, *p, 0xFFCDD6F4);
        cx += 8;
        p++;
    }
}

// -----------------------------------------------------------------------------
// App Entry Point
// -----------------------------------------------------------------------------

void run() {
    if (!Kernel::Drivers::Framebuffer::valid()) return;

    uint32_t actual_w = 0, actual_h = 0;
    if (!Kernel::AppCommon::request_window_size(600, 460, actual_w, actual_h)) return;
    const int32_t w = static_cast<int32_t>(actual_w);
    const int32_t h = static_cast<int32_t>(actual_h);
    if (w < 300 || h < 200) return;

    Kernel::Graphics::VirtualFramebuffer vfb(
        0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    if (!vfb.valid()) return;

    char expression[96] = "";
    char display[64] = "0";
    bool evaluated = false;
    bool error = false;
    bool running = true;

    while (running && !Kernel::Scheduler::should_stop()) {
        while (Keyboard::available()) {
            char c = Keyboard::getchar();

            if (c == 'q' || c == 27) { running = false; break; }

            if (c == 'c' || c == 'C') {
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
                if (strlen(expression) == 0) continue;
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
                c == '*' || c == '/' || c == '(' || c == ')' || c == '^' || c == ',') {

                if (evaluated) {
                    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
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

        if (!running) break;

        // Dark theme background (Catppuccin Mocha)
        vfb.clear(0xFF1E1E2E);

        // Borderless filled block display panel
        vfb.fill_rect(20, 20, w - 40, 60, 0xFF313244);
        if (strlen(expression) > 0) {
            draw_highlighted_expression(vfb, 32, 42, expression);
        } else {
            text(vfb, 32, 42, display, error ? 0xFFF38BA8 : 0xFFCDD6F4);
        }

        // Minimalist help guide
        text(vfb, 20, 100, "Functions: sin, cos, tan, sqrt, root, log10, logn, pi", 0xFF6C7086);
        text(vfb, 20, 118, "Exponents: ^-1, ^-2 | Enter: = | C: Clear | Q: Quit", 0xFF6C7086);

        vfb.flush();
        delay(10);
    }
}

} // namespace Kernel::Apps::Calc
