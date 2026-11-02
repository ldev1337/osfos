#include <kernel/cpu.h>
#include <kernel/drivers/framebuffer.h>
#include <kernel/drivers/mouse.h>

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64

#define MOUSE_CMD_ENABLE 0xF4
#define MOUSE_CMD_RESET 0xFF

namespace Kernel::Drivers {

uint8_t Mouse::cycle_ = 0;
int8_t Mouse::bytes_[3] = {0, 0, 0};
int Mouse::x_ = 400;
int Mouse::y_ = 300;
bool Mouse::left_pressed_ = false;
bool Mouse::right_pressed_ = false;
bool Mouse::middle_pressed_ = false;

void Mouse::wait(uint8_t type) {
  uint32_t timeout = 100000;
  if (type == 0) {
    while (timeout--) {
      if ((inb(PS2_STATUS_PORT) & 2) == 0)
        return;
    }
  } else {
    while (timeout--) {
      if ((inb(PS2_STATUS_PORT) & 1) == 1)
        return;
    }
  }
}

void Mouse::write_cmd(uint8_t write) {
  wait(0);
  outb(PS2_COMMAND_PORT, 0xD4);
  wait(0);
  outb(PS2_DATA_PORT, write);
}

uint8_t Mouse::read_data() {
  wait(1);
  return inb(PS2_DATA_PORT);
}

void Mouse::init() {
  uint8_t status;

  for (int i = 0; i < 1000; ++i) {
    if (!(inb(PS2_STATUS_PORT) & 1))
      break;
    inb(PS2_DATA_PORT);
  }

  wait(0);
  outb(PS2_COMMAND_PORT, 0xA8);

  wait(0);
  outb(PS2_COMMAND_PORT, 0x20);
  wait(1);
  status = inb(PS2_DATA_PORT);
  status |= 0x03;  // Enable both keyboard (IRQ1) and mouse (IRQ12) interrupts
  status &= ~0x30; // Enable both keyboard and mouse clocks
  status |= 0x40;  // Enable keyboard translation

  wait(0);
  outb(PS2_COMMAND_PORT, 0x60);
  wait(0);
  outb(PS2_DATA_PORT, status);

  write_cmd(MOUSE_CMD_RESET);
  read_data();
  read_data();
  read_data();

  write_cmd(MOUSE_CMD_ENABLE);
  read_data();
}

void Mouse::handle_interrupt() {
  uint8_t status = inb(PS2_STATUS_PORT);

  if (!(status & 0x01) || !(status & 0x20)) {
    return;
  }

  uint8_t data = inb(PS2_DATA_PORT);

  switch (cycle_) {
  case 0:
    bytes_[0] = data;
    if (!(data & 0x08))
      return;
    cycle_++;
    break;
  case 1:
    bytes_[1] = data;
    cycle_++;
    break;
  case 2:
    bytes_[2] = data;
    cycle_ = 0;

    left_pressed_ = bytes_[0] & 0x01;
    right_pressed_ = (bytes_[0] & 0x02) >> 1;
    middle_pressed_ = (bytes_[0] & 0x04) >> 2;

    int x_rel = bytes_[1];
    int y_rel = bytes_[2];

    if (bytes_[0] & 0x10)
      x_rel |= 0xFFFFFF00;
    if (bytes_[0] & 0x20)
      y_rel |= 0xFFFFFF00;

    x_ += x_rel;
    y_ -= y_rel;

    int max_w = Drivers::Framebuffer::width();
    int max_h = Drivers::Framebuffer::height();
    if (max_w == 0)
      max_w = 800;
    if (max_h == 0)
      max_h = 600;

    if (x_ < 0)
      x_ = 0;
    if (x_ >= max_w)
      x_ = max_w - 1;
    if (y_ < 0)
      y_ = 0;
    if (y_ >= max_h)
      y_ = max_h - 1;

    break;
  }
}

} // namespace Kernel::Drivers