#include "doomgeneric/doomkeys.h"
#include <cstddef>
#include <cstdint>
#include <oslib/keyboard.h>
#include <kernel/drivers/tsc_deadline.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/lwm.h>
#include <kernel/graphics/vfb.h>
#include <oslib/ui.h>
#include <kernel/scheduler.h>

#define fopen(path, mode) kernel_vfs_open(path, mode)
#define fread(ptr, size, nmemb, stream)                                        \
  kernel_vfs_read(ptr, size, nmemb, stream)
#define fwrite(ptr, size, nmemb, stream)                                       \
  kernel_vfs_write(ptr, size, nmemb, stream)
#define fclose(stream) kernel_vfs_close(stream)
#define fseek(stream, offset, whence) kernel_vfs_seek(stream, offset, whence)
#define ftell(stream) kernel_vfs_tell(stream)
#define rewind(stream) kernel_vfs_seek(stream, 0, 0)
#define feof(stream) (kernel_vfs_tell(stream) >= (long)kernel_vfs_size(stream))

#define KEY_RIGHTARROW 0xae
#define KEY_LEFTARROW 0xac
#define KEY_UPARROW 0xad
#define KEY_DOWNARROW 0xaf
#define KEY_STRAFE_L 0xa0
#define KEY_STRAFE_R 0xa1
#define KEY_USE 0xa2
#define KEY_FIRE 0xa3
#define KEY_ESCAPE 27
#define KEY_ENTER 13
#define KEY_TAB 9
#define KEY_F1 (0x80 + 0x3b)
#define KEY_F2 (0x80 + 0x3c)
#define KEY_F3 (0x80 + 0x3d)
#define KEY_F4 (0x80 + 0x3e)
#define KEY_F5 (0x80 + 0x3f)
#define KEY_F6 (0x80 + 0x40)
#define KEY_F7 (0x80 + 0x41)
#define KEY_F8 (0x80 + 0x42)
#define KEY_F9 (0x80 + 0x43)
#define KEY_F10 (0x80 + 0x44)
#define KEY_F11 (0x80 + 0x57)
#define KEY_F12 (0x80 + 0x58)
#define KEY_BACKSPACE 0x7f
#define KEY_PAUSE 0xff
#define KEY_EQUALS 0x3d
#define KEY_MINUS 0x2d
#define KEY_RSHIFT (0x80 + 0x36)
#define KEY_RCTRL (0x80 + 0x1d)
#define KEY_RALT (0x80 + 0x38)
#define KEY_LALT KEY_RALT
#define KEY_CAPSLOCK (0x80 + 0x3a)
#define KEY_NUMLOCK (0x80 + 0x45)
#define KEY_SCRLCK (0x80 + 0x46)
#define KEY_PRTSCR (0x80 + 0x59)
#define KEY_HOME (0x80 + 0x47)
#define KEY_END (0x80 + 0x4f)
#define KEY_PGUP (0x80 + 0x49)
#define KEY_PGDN (0x80 + 0x51)
#define KEY_INS (0x80 + 0x52)
#define KEY_DEL (0x80 + 0x53)
#define KEYP_0 0
#define KEYP_1 KEY_END
#define KEYP_2 KEY_DOWNARROW
#define KEYP_3 KEY_PGDN
#define KEYP_4 KEY_LEFTARROW
#define KEYP_5 '5'
#define KEYP_6 KEY_RIGHTARROW
#define KEYP_7 KEY_HOME
#define KEYP_8 KEY_UPARROW
#define KEYP_9 KEY_PGUP
#define KEYP_DIVIDE '/'
#define KEYP_PLUS '+'
#define KEYP_MINUS '-'
#define KEYP_MULTIPLY '*'
#define KEYP_PERIOD 0
#define KEYP_EQUALS KEY_EQUALS
#define KEYP_ENTER KEY_ENTER

struct KeyMapping {
  uint8_t scancode;
  bool extended;
  unsigned char doom_key;
};

extern "C" {
extern unsigned int *DG_ScreenBuffer;
void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick();
void doomgeneric_Shutdown();
void I_Quit();

static volatile bool doom_quit_requested = false;

void DG_RequestQuit() { doom_quit_requested = true; }

static Kernel::Graphics::VirtualFramebuffer *doom_window = nullptr;
static uint32_t doom_width = 640, doom_height = 400;

void DG_Init() {
  if (doom_window && doom_window->open_window(640, 400)) {
    doom_width = doom_window->width();
    doom_height = doom_window->height();
  }
}

void DG_DrawFrame() {
  if (doom_window && doom_window->valid()) {
    if (doom_width == 640 && doom_height == 400 &&
        Kernel::Graphics::WindowManager::present(
            Kernel::Scheduler::current_thread_id(), DG_ScreenBuffer, 640, 400,
            640 * sizeof(uint32_t))) {
      return;
    }

    uint32_t *dst = doom_window->buffer();
    for (uint32_t y = 0; y < doom_height; ++y) {
      const auto sy =
          static_cast<uint32_t>(static_cast<uint64_t>(y) * 400 / doom_height);
      for (uint32_t x = 0; x < doom_width; ++x) {
        const auto sx =
            static_cast<uint32_t>(static_cast<uint64_t>(x) * 640 / doom_width);
        dst[static_cast<size_t>(y) * doom_width + x] =
            DG_ScreenBuffer[static_cast<size_t>(sy) * 640 + sx];
      }
    }
    doom_window->flush();
  }
}
void DG_SleepMs(uint32_t ms) { Kernel::Scheduler::thread_sleep_ms(ms); }

uint32_t DG_GetTicksMs() { return Kernel::Drivers::TSCDeadline::get_ticks(); }

int DG_GetKey(int *pressed, unsigned char *doomKey) {
  // PS/2 Scancode Set 1 -> DOOM keycode (0 = unmapped)
  // DOOM expects lowercase ASCII for letters/symbols and KEY_* for specials.
  static constexpr unsigned char normal_keymap[128] = {
      /* 0x00 */ 0,
      /* 0x01 */ KEY_ESCAPE,
      /* 0x02 */ '1',
      /* 0x03 */ '2',
      /* 0x04 */ '3',
      /* 0x05 */ '4',
      /* 0x06 */ '5',
      /* 0x07 */ '6',
      /* 0x08 */ '7',
      /* 0x09 */ '8',
      /* 0x0A */ '9',
      /* 0x0B */ '0',
      /* 0x0C */ KEY_MINUS,
      /* 0x0D */ KEY_EQUALS,
      /* 0x0E */ KEY_BACKSPACE,
      /* 0x0F */ KEY_TAB,
      /* 0x10 */ 'q',
      /* 0x11 */ 'w',
      /* 0x12 */ 'e',
      /* 0x13 */ 'r',
      /* 0x14 */ 't',
      /* 0x15 */ 'y',
      /* 0x16 */ 'u',
      /* 0x17 */ 'i',
      /* 0x18 */ 'o',
      /* 0x19 */ 'p',
      /* 0x1A */ '[',
      /* 0x1B */ ']',
      /* 0x1C */ KEY_ENTER,
      /* 0x1D */ KEY_RCTRL, // Left Ctrl -> Fire
      /* 0x1E */ 'a',
      /* 0x1F */ 's',
      /* 0x20 */ 'd',
      /* 0x21 */ 'f',
      /* 0x22 */ 'g',
      /* 0x23 */ 'h',
      /* 0x24 */ 'j',
      /* 0x25 */ 'k',
      /* 0x26 */ 'l',
      /* 0x27 */ ';',
      /* 0x28 */ '\'',
      /* 0x29 */ '`',
      /* 0x2A */ KEY_RSHIFT, // Left Shift -> Run
      /* 0x2B */ '\\',
      /* 0x2C */ 'z',
      /* 0x2D */ 'x',
      /* 0x2E */ 'c',
      /* 0x2F */ 'v',
      /* 0x30 */ 'b',
      /* 0x31 */ 'n',
      /* 0x32 */ 'm',
      /* 0x33 */ ',',
      /* 0x34 */ '.',
      /* 0x35 */ '/',
      /* 0x36 */ KEY_RSHIFT, // Right Shift -> Run
      /* 0x37 */ KEYP_MULTIPLY,
      /* 0x38 */ KEY_RALT, // Left Alt -> Strafe modifier
      /* 0x39 */ ' ',      // Space -> Use / Open doors
      /* 0x3A */ KEY_CAPSLOCK,
      /* 0x3B */ KEY_F1,
      /* 0x3C */ KEY_F2,
      /* 0x3D */ KEY_F3,
      /* 0x3E */ KEY_F4,
      /* 0x3F */ KEY_F5,
      /* 0x40 */ KEY_F6,
      /* 0x41 */ KEY_F7,
      /* 0x42 */ KEY_F8,
      /* 0x43 */ KEY_F9,
      /* 0x44 */ KEY_F10,
      /* 0x45 */ KEY_NUMLOCK,
      /* 0x46 */ KEY_SCRLCK,
      /* 0x47 */ KEYP_7,
      /* 0x48 */ KEYP_8,
      /* 0x49 */ KEYP_9,
      /* 0x4A */ KEYP_MINUS,
      /* 0x4B */ KEYP_4,
      /* 0x4C */ KEYP_5,
      /* 0x4D */ KEYP_6,
      /* 0x4E */ KEYP_PLUS,
      /* 0x4F */ KEYP_1,
      /* 0x50 */ KEYP_2,
      /* 0x51 */ KEYP_3,
      /* 0x52 */ KEYP_0,
      /* 0x53 */ KEYP_PERIOD,
      /* 0x54 */ 0,
      /* 0x55 */ 0,
      /* 0x56 */ '\\',
      /* 0x57 */ KEY_F11,
      /* 0x58 */ KEY_F12,
  };

  // E0-prefixed Set 1 scancodes -> DOOM keycode
  static constexpr unsigned char extended_keymap[128] = {
      /* 0x1C */ [0x1C] = KEYP_ENTER,
      /* 0x1D */ [0x1D] = KEY_RCTRL,
      /* 0x35 */ [0x35] = KEYP_DIVIDE,
      /* 0x37 */ [0x37] = KEY_PRTSCR,
      /* 0x38 */ [0x38] = KEY_RALT,
      /* 0x47 */ [0x47] = KEY_HOME,
      /* 0x48 */ [0x48] = KEY_UPARROW,
      /* 0x49 */ [0x49] = KEY_PGUP,
      /* 0x4B */ [0x4B] = KEY_LEFTARROW,
      /* 0x4D */ [0x4D] = KEY_RIGHTARROW,
      /* 0x4F */ [0x4F] = KEY_END,
      /* 0x50 */ [0x50] = KEY_DOWNARROW,
      /* 0x51 */ [0x51] = KEY_PGDN,
      /* 0x52 */ [0x52] = KEY_INS,
      /* 0x53 */ [0x53] = KEY_DEL,
  };

  static bool last_states[128] = {false};
  static bool last_ext_states[128] = {false};

  for (uint8_t sc = 1; sc < 128; ++sc) {
    const unsigned char dk = normal_keymap[sc];
    if (dk != 0) {
      const bool current = oslib::Keyboard::is_pressed(sc);
      if (current != last_states[sc]) {
        last_states[sc] = current;
        *pressed = current ? 1 : 0;
        *doomKey = dk;
        return 1;
      }
    }
  }

  for (uint8_t sc = 1; sc < 128; ++sc) {
    const unsigned char dk = extended_keymap[sc];
    if (dk != 0) {
      const bool current = oslib::Keyboard::is_extended_pressed(sc);
      if (current != last_ext_states[sc]) {
        last_ext_states[sc] = current;
        *pressed = current ? 1 : 0;
        *doomKey = dk;
        return 1;
      }
    }
  }

  return 0;
}

void DG_SetWindowTitle(const char * /*unused*/) {}
}

static void show_startup_error(bool disk_not_initialized) {
  if (!doom_window || !doom_window->open_window(640, 400)) {
    return;
  }

  doom_window->fill(0xFF17191D);
  doom_window->fill_rect(24, 72, 4, 56, 0xFFE05A47);

  const char *title = "DOOM COULD NOT START";
  const char *message = disk_not_initialized ? "Disk is not initialized."
                                             : "WAD file was not found.";
  UI::TextObject title_text(44, 76, title, 0xFFFFC857);
  UI::TextObject message_text(44, 100, message, 0xFFE8E8E8);
  title_text.set_font_size_pixels(16.0f);
  message_text.set_font_size_pixels(14.0f);
  title_text.draw(*doom_window);
  message_text.draw(*doom_window);

  doom_window->flush();
}

namespace Kernel::Games::Doom {
void run(const char *wad) {
  Kernel::Graphics::VirtualFramebuffer window;
  doom_window = &window;
  if (!Kernel::FS::VFS::is_mounted()) {
    show_startup_error(true);
    while (!Kernel::Scheduler::should_stop()) {
      Kernel::Scheduler::thread_sleep_ms(50);
    }
    doom_window = nullptr;
    return;
  }

  void *wad_file =
      wad != nullptr && wad[0] != '\0' ? kernel_vfs_open(wad, "r") : nullptr;
  if (wad_file == nullptr) {
    show_startup_error(false);
    while (!Kernel::Scheduler::should_stop()) {
      Kernel::Scheduler::thread_sleep_ms(50);
    }
    doom_window = nullptr;
    return;
  }
  kernel_vfs_close(wad_file);

  char *argv[] = {
      (char *)"doom",
      (char *)"-iwad",
      const_cast<char *>(wad),
      (char *)"-mb",
      (char *)"3",
      (char *)"-savedir",
      (char *)"/bin/doom/saves",
  };
  doom_quit_requested = false;
  doomgeneric_Create(sizeof(argv) / sizeof(argv[0]), argv);
  bool running = true;
  while (running && !doom_quit_requested && !Kernel::Scheduler::should_stop()) {
    doomgeneric_Tick();
  }
  I_Quit();
  doomgeneric_Shutdown();
  doom_window = nullptr;
}
} // namespace Kernel::Games::Doom
