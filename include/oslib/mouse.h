#pragma once

namespace oslib {

class Mouse {
public:
  static int get_x();
  static int get_y();
  static bool is_left_pressed();
  static bool is_right_pressed();
  static bool is_middle_pressed();
};

} // namespace oslib

namespace OS {
using Mouse = oslib::Mouse;
} // namespace OS

using oslib::Mouse;
