#include <atomic>
#include <cstddef>
#include <kernel/debug/debug.h>

namespace Kernel::Debug {
namespace {
constexpr uint16_t COM1 = 0x3F8;
constexpr size_t VGA_WIDTH = 80;
constexpr size_t VGA_HEIGHT = 25;
constexpr uint8_t VGA_BACKGROUND = 1;
constexpr uint8_t VGA_TEXT = 15;
constexpr uint8_t VGA_INFO = 11;
std::atomic_flag output_lock = ATOMIC_FLAG_INIT;
bool uart_present = false;
volatile uint16_t *const vga = reinterpret_cast<volatile uint16_t *>(0xB8000);
size_t vga_x = 0, vga_y = 0;
LogEntry log_ring[LOG_CAPACITY]{};
uint64_t log_sequence = 0;

uint64_t timestamp() {
  uint32_t low, high;
  __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
  return (static_cast<uint64_t>(high) << 32) | low;
}

void remember(const char *message) {
  LogEntry &entry = log_ring[log_sequence % LOG_CAPACITY];
  entry.timestamp = timestamp();
  size_t i = 0;
  if (message) {
    while (message[i] && i + 1 < LOG_MESSAGE_SIZE) {
      entry.message[i] = message[i];
      ++i;
    }
  }
  entry.message[i] = '\0';
  ++log_sequence;
}

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
  remember(label);
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
  outb(COM1 + 1, 0x00);
  outb(COM1 + 3, 0x80);
  outb(COM1 + 0, 0x01);
  outb(COM1 + 1, 0x00);
  outb(COM1 + 3, 0x03);
  outb(COM1 + 2, 0xC7);
  outb(COM1 + 4, 0x0B);
  outb(COM1 + 7, 0xA5);
  uart_present = inb(COM1 + 7) == 0xA5;
  log(uart_present ? "debug output ready (COM1 115200 8N1)"
                   : "debug output ready (VGA; COM1 unavailable)");
}

void log(const char *message) {
  acquire();
  remember(message);
  const bool benchmark = message && message[0] == '[' && message[1] == 'B' &&
                         message[2] == ']';
  if (!benchmark)
    log_prefix();
  vga_write(message, VGA_TEXT);
  vga_put('\n', VGA_TEXT);
  if (uart_present) {
    if (!benchmark)
      write_unlocked("[I] ");
    write_unlocked(message);
    write_unlocked("\n");
  }
  release();
}
void warn(const char *message) {
  acquire();
  remember(message);
  vga_write("[W] ", VGA_INFO);
  vga_write(message, VGA_TEXT);
  vga_put('\n', VGA_TEXT);
  if (uart_present) { write_unlocked("[W] "); write_unlocked(message); write_unlocked("\n"); }
  release();
}
void error(const char *message) {
  acquire();
  remember(message);
  vga_write("[E] ", VGA_INFO);
  vga_write(message, VGA_TEXT);
  vga_put('\n', VGA_TEXT);
  if (uart_present) { write_unlocked("[E] "); write_unlocked(message); write_unlocked("\n"); }
  release();
}
void log_hex(const char *label, uint64_t value) { log_line(label, value, 16, true); }
void log_dec(const char *label, uint64_t value) { log_line(label, value, 10, false); }

size_t log_count() {
  acquire();
  const size_t count = log_sequence < LOG_CAPACITY ? log_sequence : LOG_CAPACITY;
  release();
  return count;
}

bool read_log(size_t oldest_index, LogEntry *entry) {
  if (!entry) return false;
  acquire();
  const uint64_t first = log_sequence > LOG_CAPACITY ? log_sequence - LOG_CAPACITY : 0;
  const uint64_t requested = first + oldest_index;
  if (requested >= log_sequence) { release(); return false; }
  *entry = log_ring[requested % LOG_CAPACITY];
  release();
  return true;
}
} // namespace Kernel::Debug
