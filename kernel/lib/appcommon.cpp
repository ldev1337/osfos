#include <arch/x86_64/string.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/open_font.h>
#include <kernel/lib/appcommon.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>

namespace {

Kernel::Graphics::TrueTypeFont &app_text_font() {
  static Kernel::Graphics::TrueTypeFont font;
  static bool initialized = false;
  if (!initialized) {
    initialized = Kernel::Graphics::OpenFont::load_preferred(font, "GeistMono.ttf",
                                                            "Geist.ttf");
  }
  return font;
}

} // namespace

namespace Kernel::AppCommon {

[[noreturn]] void exit(int status) {
  (void)status;
  const uint64_t thread_id = Kernel::Scheduler::current_thread_id();
  Kernel::Graphics::WindowManager::unregister_window(thread_id);
  Kernel::Memory::g_kheap.free_thread_allocations(thread_id);
  Kernel::Scheduler::thread_exit();
}

void text(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x, int32_t y,
          const char *s, uint32_t color, float font_size_pixels) {
  if (!s || !*s || !vfb.valid())
    return;
  if (font_size_pixels <= 0.0f)
    return;
  auto &font = app_text_font();
  if (font.is_valid()) {
    vfb.draw_string(x, y, font_size_pixels, s, color);
    return;
  }
  vfb.draw_string(x, y, font_size_pixels, s, color);
}

int text_width(const char *s, float font_size_pixels) {
  if (!s || font_size_pixels <= 0.0f)
    return 0;
  auto &font = app_text_font();
  if (font.is_valid())
    return font.get_text_width(font_size_pixels, s);

  int width = 0;
  const int glyph_size = static_cast<int>(font_size_pixels + 0.5f);
  const int advance = glyph_size > 0 ? glyph_size : 1;
  while (*s++)
    width += advance;
  return width;
}

void number(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x, int32_t y,
            int32_t n, uint32_t color) {
  char digits[16];
  const int value = (n < 0) ? -n : n;
  int len = 0;
  int v = value;
  do {
    digits[len++] = static_cast<char>('0' + (v % 10));
    v /= 10;
  } while (v > 0);
  if (n < 0 && len < sizeof(digits))
    digits[len++] = '-';
  for (int i = 0; i < len; ++i) {
    const char ch = digits[len - 1 - i];
    vfb.draw_glyph(x + i * 8, y, ch, color);
  }
}

void delay(uint32_t speed) {
  if (!speed)
    speed = 1;
  const uint32_t target_ticks =
      (1000 / (60 * speed)) ? (1000 / (60 * speed)) : 1;
  Kernel::Scheduler::thread_sleep_ms(target_ticks);
}

bool get_window_position(uint64_t thread_id, int *x, int *y) {
  return Kernel::Graphics::WindowManager::get_window_position(thread_id, x, y);
}

static int mouse_position(bool horizontal) {
  int wx = 0, wy = 0;
  int position = horizontal ? Kernel::Drivers::Mouse::get_x()
                            : Kernel::Drivers::Mouse::get_y();
  uint64_t tid = Kernel::Scheduler::current_thread_id();
  return get_window_position(tid, &wx, &wy) ? position - (horizontal ? wx : wy)
                                            : position;
}

int get_mouse_x() { return mouse_position(true); }
int get_mouse_y() { return mouse_position(false); }

bool is_left_pressed() { return Kernel::Drivers::Mouse::is_left_pressed(); }

} // namespace Kernel::AppCommon