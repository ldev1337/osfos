#ifndef KERNEL_DRIVERS_MOUSE_H
#define KERNEL_DRIVERS_MOUSE_H

#include <stdint.h>

namespace Kernel::Drivers {

class Mouse {
public:
    static void init();
    static void handle_interrupt();

    static int get_x() { return x_; }
    static int get_y() { return y_; }
    static bool is_left_pressed() { return left_pressed_; }
    static bool is_right_pressed() { return right_pressed_; }
    static bool is_middle_pressed() { return middle_pressed_; }

private:
    static void wait(uint8_t type);
    static void write_cmd(uint8_t cmd);
    static uint8_t read_data();

    static uint8_t cycle_;
    static int8_t bytes_[3];
    static int x_;
    static int y_;
    static bool left_pressed_;
    static bool right_pressed_;
    static bool middle_pressed_;
};

} 

#endif