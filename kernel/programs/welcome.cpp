#include <atomic>
#include <cstdint>
#include <cstddef>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/pit.h>
#include <kernel/graphics/simplefonts.h>
#include <kernel/programs/welcome.h>
namespace Kernel::Programs {
namespace {
constexpr size_t VGA_WIDTH = 80, VGA_HEIGHT = 25;
constexpr uint8_t VGA_BACKGROUND = 1, VGA_TEXT = 15, VGA_INFO = 11;
std::atomic_flag screen_lock = ATOMIC_FLAG_INIT;
volatile uint16_t *const vga = reinterpret_cast<volatile uint16_t *>(0xB8000);
size_t vga_x = 0, vga_y = 0;
static inline void outb(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
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

void draw_entry_box(const char *text) {
  if (!Kernel::Drivers::Framebuffer::valid())
    return;
  const uint32_t width = Kernel::Drivers::Framebuffer::width();
  const uint32_t height = Kernel::Drivers::Framebuffer::height();
  if (width < 64 || height < 64)
    return;
  const uint32_t panel_w = width < 720 ? width - 32 : 688;
  const uint32_t panel_h = height < 280 ? height - 32 : 248;
  const uint32_t panel_x = (width - panel_w) / 2;
  const uint32_t panel_y = (height - panel_h) / 2;
  const uint32_t box_w = panel_w > 48 ? panel_w - 48 : panel_w;
  const uint32_t box_x = panel_x + (panel_w - box_w) / 2;
  const uint32_t box_y = panel_y + panel_h / 2 + 82;
  Kernel::Drivers::Framebuffer::fill_rect(box_x, box_y, box_w, 28, 0x07111F);
  Kernel::Drivers::Framebuffer::fill_rect(box_x, box_y, box_w, 1, 0x31C7D5);
  Kernel::Drivers::Framebuffer::fill_rect(box_x, box_y + 27, box_w, 1,
                                          0x31C7D5);
  uint32_t x = box_x + 12;
  for (size_t i = 0; text[i] && x + 8 <= box_x + box_w - 8; ++i, x += 8)
    draw_glyph(x, box_y + 10, text[i], 0xF1F5F8, 1);
  draw_glyph(x, box_y + 10, '_', 0x66D9E8, 1);
}

void vga_clear() {
  for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i)
    vga[i] = static_cast<uint16_t>(vga_attr(VGA_TEXT) << 8) | ' ';
  outb(0x3D4, 0x0A);
  outb(0x3D5, 0x20); // Hide the hardware text cursor.
  vga_x = vga_y = 0;
}

void acquire() { while (screen_lock.test_and_set(std::memory_order_acquire)) __asm__ volatile("pause"); }
void release() { screen_lock.clear(std::memory_order_release); }


} // namespace
void show() {
  acquire();
  vga_clear();
  const char *title = "Welcome to OSFOS!";
  const size_t title_x = (VGA_WIDTH - 17) / 2;
  for (size_t i = 0; title[i]; ++i)
    vga[10 * VGA_WIDTH + title_x + i] =
        static_cast<uint16_t>(vga_attr(i < 7 ? 14 : 11) << 8) | title[i];
  const char *caption = "Type hello osfos to continue";
  const size_t caption_x = (VGA_WIDTH - 28) / 2;
  for (size_t i = 0; caption[i]; ++i)
    vga[13 * VGA_WIDTH + caption_x + i] =
        static_cast<uint16_t>(vga_attr(VGA_TEXT) << 8) | caption[i];
  for (size_t x = 24; x < 56; ++x)
    vga[12 * VGA_WIDTH + x] =
        static_cast<uint16_t>(vga_attr(VGA_INFO) << 8) | 0xC4;
  vga_x = 0;
  vga_y = 15;
  vga_write("Type hello osfos to enter: ", VGA_INFO);

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
      draw_centered("Type hello osfos to continue", panel_y + panel_h / 2 + 60,
                    1, 0xAAB9C8);
      draw_entry_box("");
    }
  }
  release();

  constexpr char required[] = "hello osfos";
  char entered[sizeof(required)]{};
  size_t length = 0;
  for (;;) {
    const char c = Kernel::Drivers::Keyboard::getchar();
    if (c == '\b') {
      if (length) {
        --length;
        entered[length] = '\0';
        acquire();
        if (vga_x)
          --vga_x;
        vga_put(' ', VGA_TEXT);
        if (vga_x)
          --vga_x;
        draw_entry_box(entered);
        release();
      }
      continue;
    }
    if (c != '\n' && c != '\r') {
      if (c < 32 || c > 126 || length + 1 >= sizeof(entered))
        continue;
      entered[length++] = c;
      entered[length] = '\0';
      acquire();
      vga_put(c, VGA_TEXT);
      draw_entry_box(entered);
      release();
      continue;
    }
    if (length == sizeof(required) - 1) {
      size_t i = 0;
      while (i < sizeof(required) && entered[i] == required[i])
        ++i;
      if (i == sizeof(required))
        break;
    }
    acquire();
    vga_put('\n', VGA_TEXT);
    vga_write("Please type hello osfos exactly: ", VGA_INFO);
    draw_entry_box("");
    release();
    length = 0;
    entered[0] = '\0';
  }
}


void wait_seconds(uint32_t seconds) {
  if (!seconds) return;
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0" : "=r"(flags));
  const uint32_t start = Kernel::Drivers::PIT::get_ticks();
  const uint32_t duration = seconds * 1000u;
  while (static_cast<uint32_t>(Kernel::Drivers::PIT::get_ticks() - start) < duration)
    __asm__ volatile("sti; hlt" ::: "memory");
  if (!(flags & (1ull << 9))) __asm__ volatile("cli" ::: "memory");
}
} // namespace Kernel::Programs
