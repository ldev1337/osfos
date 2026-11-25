#pragma once

#include <cstdint>

namespace oslib {

class Keyboard {
public:
  static bool available();
  static char getchar();

  static bool is_pressed(uint8_t scancode);
  static bool is_extended_pressed(uint8_t scancode);

  static bool is_shift_pressed();
  static bool is_ctrl_pressed();
  static bool is_alt_pressed();
  static bool is_caps_lock_active();
  static bool is_num_lock_active();
  static bool is_scroll_lock_active();
};

} // namespace oslib

namespace OS {
using Keyboard = oslib::Keyboard;
} // namespace OS

using oslib::Keyboard;
