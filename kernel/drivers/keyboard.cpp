#include <kernel/cpu.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/graphics/lwm.h>
#include <kernel/init.h>
#include <kernel/scheduler.h>
#include <arch/x86_64/spinlock.h>

namespace Kernel::Drivers {

static Spinlock keyboard_lock;

static bool keyboard_focus_allows_input() {
  const uint64_t tid = Kernel::Scheduler::current_thread_id();
  return !Kernel::Graphics::WindowManager::has_window(tid) ||
         Kernel::Graphics::WindowManager::has_keyboard_focus(tid);
}

bool Keyboard::rx_buffer_empty() {
  SpinlockGuard guard(keyboard_lock);
  const uint64_t thread_id = Kernel::Scheduler::current_thread_id();
  if (rx_buffer_owner != 0 && rx_buffer_owner != thread_id)
    rx_buffer.clear();
  return rx_buffer.empty();
}

Keyboard::RingBuffer Keyboard::rx_buffer = {{0}, 0, 0};
uint64_t Keyboard::rx_buffer_owner = 0;

bool Keyboard::shift_pressed = false;
bool Keyboard::right_shift_pressed = false;
bool Keyboard::ctrl_pressed = false;
bool Keyboard::right_ctrl_pressed = false;
bool Keyboard::alt_pressed = false;
bool Keyboard::right_alt_pressed = false;
bool Keyboard::caps_lock_active = false;
bool Keyboard::num_lock_active = true;
bool Keyboard::scroll_lock_active = false;
bool Keyboard::extended_code = false;
bool Keyboard::pause_state = false;

bool Keyboard::key_states[128] = {false};
bool Keyboard::extended_key_states[128] = {false};

const char kbd_US[128] = {
    0,    27,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-',  '=',
    '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[',  ']',
    '\n', 0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,    '*',
    0,    ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,
    0,    0,    '-', 0,   0,   0,   '+', 0,   0,   0,   0,   0,   0,    0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,
    0,    0,    0,   0,   0,   0,   0,   0};

// Standard US Layout (Set 1) - Shifted
const char kbd_US_sft[128] = {
    0,    27,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    '\n', 0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    '|',  'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*',
    0,    ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,    '-', 0,   0,   0,   '+', 0,   0,   0,   0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0};

// Extended Scancode Map (E0 prefixed keys) -> ASCII or Special mapping codes
const char kbd_US_extended[128] = {
    0, 0, 0, 0, 0, 0, 0,    0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, '\n', 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,    0, 0, '/', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,    0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,    0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,    0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0};

// Special scancodes
constexpr uint8_t SCANCODE_LSHIFT = 0x2A;
constexpr uint8_t SCANCODE_RSHIFT = 0x36;
constexpr uint8_t SCANCODE_LCTRL = 0x1D;
constexpr uint8_t SCANCODE_LALT = 0x38;
constexpr uint8_t SCANCODE_CAPSLOCK = 0x3A;
constexpr uint8_t SCANCODE_NUMLOCK = 0x45;
constexpr uint8_t SCANCODE_SCROLLLOCK = 0x46;
constexpr uint8_t SCANCODE_EXTENDED = 0xE0;
constexpr uint8_t SCANCODE_PAUSE1 = 0xE1;

void __init Keyboard::init() {
  for (int i = 0; i < 10000; ++i) {
    if (!(inb(0x64) & 0x01))
      break;
    inb(0x60);
  }

  int timeout = 100000;
  while ((inb(0x64) & 0x02) && --timeout)
    ;

  if (!timeout)
    return;

  outb(0x64, 0x20);

  timeout = 100000;
  while (!(inb(0x64) & 0x01) && --timeout)
    ;

  if (!timeout)
    return;

  uint8_t command = inb(0x60);
  command |= 0x01;  // Enable first-port IRQ.
  command &= ~0x10; // Enable first-port clock.
  command |= 0x40;  // Enable first-port translation (Scancode Set 1).

  timeout = 100000;
  while ((inb(0x64) & 0x02) && --timeout)
    ;

  if (!timeout)
    return;

  outb(0x64, 0x60);

  timeout = 100000;
  while ((inb(0x64) & 0x02) && --timeout)
    ;

  if (!timeout)
    return;

  outb(0x60, command);

  // Enable First PS/2 Port (0xAE)
  timeout = 100000;
  while ((inb(0x64) & 0x02) && --timeout)
    ;
  outb(0x64, 0xAE);

  // Enable Keyboard Scanning (0xF4)
  timeout = 100000;
  while ((inb(0x64) & 0x02) && --timeout)
    ;
  outb(0x60, 0xF4);

  // Wait for ACK (0xFA)
  timeout = 100000;
  while (!(inb(0x64) & 0x01) && --timeout)
    ;
  if (timeout)
    inb(0x60);

  // Send LED state command to keyboard to synchronize Lock LEDs
  update_leds();

  // Unmask IRQ1 on the master PIC.
  uint8_t mask = inb(0x21);
  outb(0x21, mask & ~0x02);
}

bool Keyboard::available() {
  if (!keyboard_focus_allows_input())
    return false;
  SpinlockGuard guard(keyboard_lock);
  const uint64_t thread_id = Kernel::Scheduler::current_thread_id();
  if (rx_buffer_owner != 0 && rx_buffer_owner != thread_id)
    rx_buffer.clear();
  bool result = !rx_buffer.empty();
  return result;
}

char Keyboard::getchar() {
  while (true) {
    if (!keyboard_focus_allows_input()) {
      Kernel::Scheduler::thread_sleep_ms(4);
      continue;
    }
    if (!Kernel::Scheduler::thread_block_if(rx_buffer_empty)) {
      SpinlockGuard guard(keyboard_lock);
      const uint64_t thread_id = Kernel::Scheduler::current_thread_id();
      if (rx_buffer_owner != 0 && rx_buffer_owner != thread_id) {
        rx_buffer.clear();
        continue;
      }
      char c = rx_buffer.pop();
      return c;
    }
  }
}

void Keyboard::handle_interrupt() {
  const uint64_t input_owner =
      Kernel::Graphics::WindowManager::keyboard_focus_thread_id();
  bool wake_waiters = false;
  {
  SpinlockGuard guard(keyboard_lock);
  uint8_t status = inb(0x64);
  if (!(status & 0x01) || (status & 0x20)) {
    return;
  }

  uint8_t scancode = inb(0x60);
  if (scancode == 0xFA || scancode == 0xFE) {
    return;
  }

  if (rx_buffer_owner != input_owner) {
    rx_buffer.clear();
    rx_buffer_owner = input_owner;
  }

  if (scancode == SCANCODE_PAUSE1) {
    pause_state = true;
    return;
  }
  if (pause_state) {
    pause_state = false;
    return;
  }

  if (scancode == SCANCODE_EXTENDED) {
    extended_code = true;
    return;
  }

  bool is_release = (scancode & 0x80) != 0;
  uint8_t base_scancode = scancode & 0x7F;

  if (extended_code) {
    if (base_scancode < 128) {
      extended_key_states[base_scancode] = !is_release;
    }
  } else {
    if (base_scancode < 128) {
      key_states[base_scancode] = !is_release;
    }
  }

  if (extended_code) {
    if (base_scancode == 0x1D) {
      right_ctrl_pressed = !is_release;
      ctrl_pressed = right_ctrl_pressed || ctrl_pressed;
      extended_code = false;
      return;
    }
    if (base_scancode == 0x38) {
      right_alt_pressed = !is_release;
      alt_pressed = right_alt_pressed || alt_pressed;
      extended_code = false;
      return;
    }
  } else {
    if (base_scancode == SCANCODE_LSHIFT) {
      shift_pressed = !is_release;
      extended_code = false;
      return;
    }
    if (base_scancode == SCANCODE_RSHIFT) {
      right_shift_pressed = !is_release;
      shift_pressed = right_shift_pressed || shift_pressed;
      extended_code = false;
      return;
    }
    if (base_scancode == SCANCODE_LCTRL) {
      ctrl_pressed = !is_release;
      extended_code = false;
      return;
    }
    if (base_scancode == SCANCODE_LALT) {
      alt_pressed = !is_release;
      extended_code = false;
      return;
    }
    if (base_scancode == SCANCODE_CAPSLOCK && !is_release) {
      caps_lock_active = !caps_lock_active;
      update_leds();
      extended_code = false;
      return;
    }
    if (base_scancode == SCANCODE_NUMLOCK && !is_release) {
      num_lock_active = !num_lock_active;
      update_leds();
      extended_code = false;
      return;
    }
    if (base_scancode == SCANCODE_SCROLLLOCK && !is_release) {
      scroll_lock_active = !scroll_lock_active;
      update_leds();
      extended_code = false;
      return;
    }
  }

  if (is_release) {
    if (!extended_code) {
      if (base_scancode == SCANCODE_LSHIFT)
        shift_pressed = false;
      if (base_scancode == SCANCODE_RSHIFT)
        right_shift_pressed = false;
      if (base_scancode == SCANCODE_LCTRL)
        ctrl_pressed = false;
      if (base_scancode == SCANCODE_LALT)
        alt_pressed = false;
    } else {
      if (base_scancode == 0x1D)
        right_ctrl_pressed = false;
      if (base_scancode == 0x38)
        right_alt_pressed = false;
    }
    shift_pressed = shift_pressed || right_shift_pressed;
    ctrl_pressed = ctrl_pressed || right_ctrl_pressed;
    alt_pressed = alt_pressed || right_alt_pressed;
    extended_code = false;
    return;
  }

  if (base_scancode < 128) {
    char ascii = 0;

    if (extended_code) {
      ascii = kbd_US_extended[base_scancode];
    } else {
      bool use_shift = shift_pressed;
      char normal_char = kbd_US[base_scancode];

      if (caps_lock_active && normal_char >= 'a' && normal_char <= 'z') {
        use_shift = !use_shift;
      }

      ascii = use_shift ? kbd_US_sft[base_scancode] : normal_char;
    }

    if (ascii != 0) {
      rx_buffer.push(ascii);
      wake_waiters = true;
    }
  }

  extended_code = false;
  }
  if (wake_waiters)
    Kernel::Scheduler::unblock_io_threads();
}

bool Keyboard::is_pressed(uint8_t scancode) {
  if (scancode >= 128)
    return false;
  if (!keyboard_focus_allows_input())
    return false;
  SpinlockGuard guard(keyboard_lock);
  return key_states[scancode];
}

bool Keyboard::is_extended_pressed(uint8_t scancode) {
  if (scancode >= 128)
    return false;
  if (!keyboard_focus_allows_input())
    return false;
  SpinlockGuard guard(keyboard_lock);
  return extended_key_states[scancode];
}

bool Keyboard::is_shift_pressed() {
  SpinlockGuard guard(keyboard_lock);
  return shift_pressed || right_shift_pressed;
}

bool Keyboard::is_ctrl_pressed() {
  SpinlockGuard guard(keyboard_lock);
  return ctrl_pressed || right_ctrl_pressed;
}

bool Keyboard::is_alt_pressed() {
  SpinlockGuard guard(keyboard_lock);
  return alt_pressed || right_alt_pressed;
}

bool Keyboard::is_caps_lock_active() {
  SpinlockGuard guard(keyboard_lock);
  return caps_lock_active;
}

bool Keyboard::is_num_lock_active() {
  SpinlockGuard guard(keyboard_lock);
  return num_lock_active;
}

bool Keyboard::is_scroll_lock_active() {
  SpinlockGuard guard(keyboard_lock);
  return scroll_lock_active;
}

void Keyboard::update_leds() {
  uint8_t leds = 0;
  if (caps_lock_active)
    leds |= (1 << 2);
  if (num_lock_active)
    leds |= (1 << 1);
  if (scroll_lock_active)
    leds |= (1 << 0);

  int timeout = 100000;
  while ((inb(0x64) & 0x02) && --timeout)
    ;
  if (!timeout)
    return;

  outb(0x60, 0xED);

  timeout = 100000;
  while (!(inb(0x64) & 0x01) && --timeout)
    ;
  if (timeout)
    inb(0x60);

  timeout = 100000;
  while ((inb(0x64) & 0x02) && --timeout)
    ;
  if (!timeout)
    return;

  outb(0x60, leds);

  timeout = 100000;
  while (!(inb(0x64) & 0x01) && --timeout)
    ;
  if (timeout)
    inb(0x60);
}

} // namespace Kernel::Drivers
