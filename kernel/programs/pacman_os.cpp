#include <cstdint>

#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/tsc_deadline.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/vfb.h>
#include <kernel/scheduler.h>

extern "C" {
void pacman_game_init(void);
void pacman_game_frame(uint32_t frame_time_ns);
void pacman_game_input(bool up, bool down, bool left, bool right, bool escape,
                       bool anykey);
void pacman_game_shutdown(void);

bool pacman_os_present(const uint32_t *pixels, uint32_t width,
                       uint32_t height);
}

namespace Kernel::Games::Pacman {
namespace {
constexpr uint32_t kGameWidth = 224;
constexpr uint32_t kGameHeight = 288;
constexpr uint32_t kWindowWidth = 448;
constexpr uint32_t kWindowHeight = 576;

Kernel::Graphics::VirtualFramebuffer window;

bool key_down(uint8_t scancode, bool extended = false) {
  return extended ? Kernel::Drivers::Keyboard::is_extended_pressed(scancode)
                  : Kernel::Drivers::Keyboard::is_pressed(scancode);
}

} // namespace

extern "C" bool pacman_os_present(const uint32_t *pixels, uint32_t width,
                                  uint32_t height) {
  if (!pixels || width != kGameWidth || height != kGameHeight ||
      !window.valid())
    return false;

  const uint32_t target_width = window.width();
  const uint32_t target_height = window.height();
  uint32_t *target = window.buffer();
  for (uint32_t y = 0; y < target_height; ++y) {
    const uint32_t source_y =
        static_cast<uint32_t>(static_cast<uint64_t>(y) * height / target_height);
    for (uint32_t x = 0; x < target_width; ++x) {
      const uint32_t source_x =
          static_cast<uint32_t>(static_cast<uint64_t>(x) * width / target_width);
      target[static_cast<size_t>(y) * target_width + x] =
          pixels[static_cast<size_t>(source_y) * width + source_x];
    }
  }
  window.flush();
  return true;
}

void run() {
  if (!window.open_window(kWindowWidth, kWindowHeight))
    return;

  pacman_game_init();
  uint32_t last_tick = Kernel::Drivers::TSCDeadline::get_ticks();
  while (!Kernel::Scheduler::should_stop() &&
         Kernel::Graphics::WindowManager::has_window(
             Kernel::Scheduler::current_thread_id())) {
    const uint32_t current_tick = Kernel::Drivers::TSCDeadline::get_ticks();
    const uint32_t frame_time_ns = (current_tick - last_tick) * 1000000u;
    last_tick = current_tick;

    const bool focused = window.has_focus();
    const bool up = focused && (key_down(0x48, true) || key_down(0x11));
    const bool down = focused && (key_down(0x50, true) || key_down(0x1F));
    const bool left = focused && (key_down(0x4B, true) || key_down(0x1E));
    const bool right = focused && (key_down(0x4D, true) || key_down(0x20));
    const bool escape = focused && key_down(0x01);
    const bool anykey = focused && (up || down || left || right || escape ||
                                    key_down(0x39) || key_down(0x1C));
    if (escape)
      break;

    pacman_game_input(up, down, left, right, false, anykey);
    pacman_game_frame(frame_time_ns);
    Kernel::Scheduler::thread_sleep_ms(2);
  }
  pacman_game_shutdown();
  window.destroy();
}
} // namespace Kernel::Games::Pacman
