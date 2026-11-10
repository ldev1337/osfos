#include <atomic>
#include <cstddef>
#include <kernel/debug.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/pit.h>
#include <kernel/graphics/simplefonts.h>

namespace Kernel::Debug {
namespace {

constexpr uint16_t COM1 = 0x3F8;
constexpr size_t VGA_WIDTH = 80;
constexpr size_t VGA_HEIGHT = 25;
constexpr uint8_t VGA_BACKGROUND = 1; // blue
constexpr uint8_t VGA_TEXT = 15;      // bright white
constexpr uint8_t VGA_INFO = 11;      // bright cyan
std::atomic_flag output_lock = ATOMIC_FLAG_INIT;
bool uart_present = false;
volatile uint16_t *const vga = reinterpret_cast<volatile uint16_t *>(0xB8000);
size_t vga_x = 0, vga_y = 0;

static inline void outb(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

uint8_t vga_attr(uint8_t foreground) {
  return static_cast<uint8_t>((VGA_BACKGROUND << 4) | foreground);
}

void vga_scroll() {
  if (vga_y < VGA_HEIGHT)
    return;
  for (size_t y = 1; y < VGA_HEIGHT; ++y)
    for (size_t x = 0; x < VGA_WIDTH; ++x)
      vga[(y - 1) * VGA_WIDTH + x] = vga[y * VGA_WIDTH + x];
  for (size_t x = 0; x < VGA_WIDTH; ++x)
    vga[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
        static_cast<uint16_t>(vga_attr(VGA_TEXT) << 8) | ' ';
  vga_y = VGA_HEIGHT - 1;
}

void vga_put(char c, uint8_t color) {
  if (c == '\r')
    return;
  if (c == '\n') {
    vga_x = 0;
    ++vga_y;
    vga_scroll();
    return;
  }
  vga[vga_y * VGA_WIDTH + vga_x] =
      static_cast<uint16_t>(vga_attr(color) << 8) | static_cast<uint8_t>(c);
  if (++vga_x == VGA_WIDTH) {
    vga_x = 0;
    ++vga_y;
    vga_scroll();
  }
}

void vga_write(const char *text, uint8_t color) {
  if (!text)
    return;
  while (*text)
    vga_put(*text++, color);
}

void put(char c) {
  // Bound the wait so a missing UART cannot stall boot forever.
  for (uint32_t tries = 0; tries < 100000; ++tries) {
    if (inb(COM1 + 5) & 0x20) {
      outb(COM1, static_cast<uint8_t>(c));
      return;
    }
    __asm__ volatile("pause");
  }
}

void write_unlocked(const char *text) {
  if (!text)
    return;
  while (*text) {
    if (*text == '\n')
      put('\r');
    put(*text++);
  }
}

void log_prefix() {
  vga_write("[I] ", VGA_INFO);
  if (uart_present)
    write_unlocked("[I] ");
}

void log_line(const char *label, uint64_t value, unsigned base,
              bool hexadecimal) {
  if (!uart_present && !label)
    return;
  while (output_lock.test_and_set(std::memory_order_acquire))
    __asm__ volatile("pause");
  log_prefix();
  vga_write(label, VGA_TEXT);
  if (uart_present)
    write_unlocked(label);
  if (hexadecimal) {
    vga_write("=0x", VGA_TEXT);
    if (uart_present)
      write_unlocked("=0x");
  } else {
    vga_write("=", VGA_TEXT);
    if (uart_present)
      write_unlocked("=");
  }
  char digits[20];
  unsigned length = 0;
  do {
    const unsigned digit = static_cast<unsigned>(value % base);
    digits[length++] =
        static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
    value /= base;
  } while (value && length < sizeof(digits));
  while (length) {
    const char c = digits[--length];
    vga_put(c, VGA_TEXT);
    if (uart_present)
      put(c);
  }
  vga_put('\n', VGA_TEXT);
  if (uart_present)
    write_unlocked("\n");
  output_lock.clear(std::memory_order_release);
}

void draw_glyph(uint32_t x, uint32_t y, char ch, uint32_t color,
                uint32_t scale) {
  const auto c = static_cast<unsigned char>(ch);
  const auto *glyph =
      Kernel::Graphics::Font::Basic8x8[(c >= 32 && c <= 127) ? c - 32
                                                             : '?' - 32];
  for (uint32_t gy = 0; gy < 8; ++gy)
    for (uint32_t gx = 0; gx < 8; ++gx)
      if (glyph[gy] & (1u << gx))
        for (uint32_t sy = 0; sy < scale; ++sy)
          for (uint32_t sx = 0; sx < scale; ++sx)
            Kernel::Drivers::Framebuffer::put_pixel(x + gx * scale + sx,
                                                    y + gy * scale + sy, color);
}

void draw_centered(const char *text, uint32_t y, uint32_t scale,
                   uint32_t color) {
  const uint32_t width = Kernel::Drivers::Framebuffer::width();
  size_t length = 0;
  while (text[length])
    ++length;
  const uint32_t text_width = static_cast<uint32_t>(length) * 8 * scale;
  uint32_t x = text_width < width ? (width - text_width) / 2 : 0;
  for (size_t i = 0; i < length; ++i, x += 8 * scale)
    draw_glyph(x, y, text[i], color, scale);
}

void vga_clear() {
  for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i)
    vga[i] = static_cast<uint16_t>(vga_attr(VGA_TEXT) << 8) | ' ';
  outb(0x3D4, 0x0A);
  outb(0x3D5, 0x20); // Hide the hardware text cursor.
  vga_x = vga_y = 0;
}

void acquire() {
  while (output_lock.test_and_set(std::memory_order_acquire))
    __asm__ volatile("pause");
}

void release() { output_lock.clear(std::memory_order_release); }

} // namespace

void init() {
  vga_clear();
  outb(COM1 + 1, 0x00); // Disable UART interrupts.
  outb(COM1 + 3, 0x80); // Divisor latch access.
  outb(COM1 + 0, 0x01); // 115200 baud.
  outb(COM1 + 1, 0x00);
  outb(COM1 + 3, 0x03); // 8 data bits, no parity, one stop bit.
  outb(COM1 + 2, 0xC7); // Enable and clear FIFO.
  outb(COM1 + 4, 0x0B); // DTR, RTS, OUT2.
  outb(COM1 + 7, 0xA5); // Scratch register detects an absent legacy UART.
  uart_present = inb(COM1 + 7) == 0xA5;
  log(uart_present ? "debug output ready (COM1 115200 8N1)"
                   : "debug output ready (VGA; COM1 unavailable)");
}

void log(const char *message) {
  acquire();
  log_prefix();
  vga_write(message, VGA_TEXT);
  vga_put('\n', VGA_TEXT);
  if (uart_present) {
    write_unlocked(message);
    write_unlocked("\n");
  }
  release();
}

void log_hex(const char *label, uint64_t value) {
  log_line(label, value, 16, true);
}

void log_dec(const char *label, uint64_t value) {
  log_line(label, value, 10, false);
}

void show_welcome() {
  acquire();
  vga_clear();
  const char *title = "Welcome to OSFOS!";
  const size_t title_x = (VGA_WIDTH - 17) / 2;
  for (size_t i = 0; title[i]; ++i)
    vga[10 * VGA_WIDTH + title_x + i] =
        static_cast<uint16_t>(vga_attr(i < 7 ? 14 : 11) << 8) | title[i];
  const char *caption = "Initializing graphical desktop...";
  const size_t caption_x = (VGA_WIDTH - 34) / 2;
  for (size_t i = 0; caption[i]; ++i)
    vga[13 * VGA_WIDTH + caption_x + i] =
        static_cast<uint16_t>(vga_attr(VGA_TEXT) << 8) | caption[i];
  for (size_t x = 24; x < 56; ++x)
    vga[12 * VGA_WIDTH + x] =
        static_cast<uint16_t>(vga_attr(VGA_INFO) << 8) | 0xC4;
  vga_x = 0;
  vga_y = 15;

  if (Kernel::Drivers::Framebuffer::valid()) {
    const uint32_t width = Kernel::Drivers::Framebuffer::width();
    const uint32_t height = Kernel::Drivers::Framebuffer::height();
    Kernel::Drivers::Framebuffer::fill(0x07111F);
    if (width >= 64 && height >= 64) {
      const uint32_t panel_w = width < 720 ? width - 32 : 688;
      const uint32_t panel_h = height < 280 ? height - 32 : 248;
      const uint32_t panel_x = (width - panel_w) / 2;
      const uint32_t panel_y = (height - panel_h) / 2;
      Kernel::Drivers::Framebuffer::fill_rect(panel_x, panel_y, panel_w,
                                              panel_h, 0x101F31);
      Kernel::Drivers::Framebuffer::fill_rect(panel_x, panel_y, panel_w, 4,
                                              0x31C7D5);
      draw_centered(title, panel_y + panel_h / 2 - 44, 3, 0xF1F5F8);
      draw_centered("OSFOS KERNEL STARTUP", panel_y + panel_h / 2 + 28, 1,
                    0x66D9E8);
      draw_centered("Initializing graphical desktop...",
                    panel_y + panel_h / 2 + 60, 1, 0xAAB9C8);
    }
  }
  release();
}

void wait_seconds(uint32_t seconds) {
  if (!seconds)
    return;
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0" : "=r"(flags));
  const uint32_t start = Kernel::Drivers::PIT::get_ticks();
  const uint32_t duration = seconds * 1000u;
  while (static_cast<uint32_t>(Kernel::Drivers::PIT::get_ticks() - start) <
         duration)
    __asm__ volatile("sti; hlt" ::: "memory");
  if (!(flags & (1ull << 9)))
    __asm__ volatile("cli" ::: "memory");
}

} // namespace Kernel::Debug
