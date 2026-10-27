#pragma once

#include <cstdint>
#include <cstddef>

namespace Kernel::Drivers {

class Keyboard {
public:
    static void init();

    static bool available();

    static char getchar();

    static void handle_interrupt();

    static bool is_pressed(uint8_t scancode);
    static bool is_extended_pressed(uint8_t scancode);

    static bool is_shift_pressed();
    static bool is_ctrl_pressed();
    static bool is_alt_pressed();
    static bool is_caps_lock_active();
    static bool is_num_lock_active();
    static bool is_scroll_lock_active();

private:
    static constexpr size_t BUFFER_SIZE = 256;

    struct RingBuffer {
        char buffer[BUFFER_SIZE];
        volatile size_t head;
        volatile size_t tail;

        bool full() const { return ((tail + 1) % BUFFER_SIZE) == head; }
        bool empty() const { return head == tail; }

        void push(char c) {
            if (!full()) {
                buffer[tail] = c;
                tail = (tail + 1) % BUFFER_SIZE;
            }
        }

        char pop() {
            if (empty()) return 0;
            char c = buffer[head];
            head = (head + 1) % BUFFER_SIZE;
            return c;
        }
    };

    static RingBuffer rx_buffer;
    static bool rx_buffer_empty();

    static bool shift_pressed;
    static bool right_shift_pressed;
    static bool ctrl_pressed;
    static bool right_ctrl_pressed;
    static bool alt_pressed;
    static bool right_alt_pressed;
    static bool caps_lock_active;
    static bool num_lock_active;
    static bool scroll_lock_active;
    static bool extended_code;
    static bool pause_state;

    static bool key_states[128];
    static bool extended_key_states[128];

    static void update_leds();
};

}
