#include <oslib/keyboard.h>
#include <oslib/mouse.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>

namespace oslib {

bool Keyboard::available() {
  return Kernel::Drivers::Keyboard::available();
}

char Keyboard::getchar() {
  return Kernel::Drivers::Keyboard::getchar();
}

bool Keyboard::is_pressed(uint8_t scancode) {
  return Kernel::Drivers::Keyboard::is_pressed(scancode);
}

bool Keyboard::is_extended_pressed(uint8_t scancode) {
  return Kernel::Drivers::Keyboard::is_extended_pressed(scancode);
}

bool Keyboard::is_shift_pressed() {
  return Kernel::Drivers::Keyboard::is_shift_pressed();
}

bool Keyboard::is_ctrl_pressed() {
  return Kernel::Drivers::Keyboard::is_ctrl_pressed();
}

bool Keyboard::is_alt_pressed() {
  return Kernel::Drivers::Keyboard::is_alt_pressed();
}

bool Keyboard::is_caps_lock_active() {
  return Kernel::Drivers::Keyboard::is_caps_lock_active();
}

bool Keyboard::is_num_lock_active() {
  return Kernel::Drivers::Keyboard::is_num_lock_active();
}

bool Keyboard::is_scroll_lock_active() {
  return Kernel::Drivers::Keyboard::is_scroll_lock_active();
}

int Mouse::get_x() {
  return Kernel::Drivers::Mouse::get_x();
}

int Mouse::get_y() {
  return Kernel::Drivers::Mouse::get_y();
}

bool Mouse::is_left_pressed() {
  return Kernel::Drivers::Mouse::is_left_pressed();
}

bool Mouse::is_right_pressed() {
  return Kernel::Drivers::Mouse::is_right_pressed();
}

bool Mouse::is_middle_pressed() {
  return Kernel::Drivers::Mouse::is_middle_pressed();
}

} // namespace oslib
