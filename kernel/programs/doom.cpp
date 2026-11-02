#include <cstddef>
#include <cstdint>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/pit.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/vfb.h>
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
#define KEY_ESCAPE 27
#define KEY_ENTER 13
#define KEY_RCTRL (0x80 + 0x1d)
#define KEY_RALT (0x80 + 0x38)

struct KeyMapping {
  uint8_t scancode;
  bool extended;
  unsigned char doom_key;
};

extern "C" {
extern unsigned int *DG_ScreenBuffer;
void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick();

static Kernel::Graphics::VirtualFramebuffer doom_window;
static uint32_t doom_width = 640, doom_height = 400;

void DG_Init() {
  if (doom_window.open_window(640, 400)) {
    doom_width = doom_window.width();
    doom_height = doom_window.height();
  }
}

void DG_DrawFrame() {
  if (doom_window.valid()) {
    uint32_t *dst = doom_window.buffer();
    for (uint32_t y = 0; y < doom_height; ++y) {
      const uint32_t sy =
          static_cast<uint32_t>(static_cast<uint64_t>(y) * 400 / doom_height);
      for (uint32_t x = 0; x < doom_width; ++x) {
        const uint32_t sx =
            static_cast<uint32_t>(static_cast<uint64_t>(x) * 640 / doom_width);
        dst[static_cast<size_t>(y) * doom_width + x] =
            DG_ScreenBuffer[static_cast<size_t>(sy) * 640 + sx];
      }
    }
    doom_window.flush();
  }
}
void DG_SleepMs(uint32_t ms) { Kernel::Scheduler::thread_sleep_ms(ms); }

uint32_t DG_GetTicksMs() { return Kernel::Drivers::PIT::get_ticks(); }

int DG_GetKey(int *pressed, unsigned char *doomKey) {
  static const KeyMapping mappings[] = {
      {0x48, true, KEY_UPARROW},   {0x50, true, KEY_DOWNARROW},
      {0x4B, true, KEY_LEFTARROW}, {0x4D, true, KEY_RIGHTARROW},
      {0x1C, false, KEY_ENTER},    {0x39, false, ' '},
      {0x1D, false, KEY_RCTRL},    {0x38, false, KEY_RALT},
      {0x01, false, KEY_ESCAPE},
  };

  static bool last_states[16] = {false};
  static bool last_ext_states[16] = {false};

  for (size_t i = 0; i < sizeof(mappings) / sizeof(mappings[0]); ++i) {
    bool current =
        mappings[i].extended
            ? Kernel::Drivers::Keyboard::is_extended_pressed(
                  mappings[i].scancode)
            : Kernel::Drivers::Keyboard::is_pressed(mappings[i].scancode);

    bool &last = mappings[i].extended ? last_ext_states[i] : last_states[i];

    if (current != last) {
      last = current;
      *pressed = current ? 1 : 0;
      *doomKey = mappings[i].doom_key;
      return 1;
    }
  }
  return 0;
}

void DG_SetWindowTitle(const char *) {}
}

namespace Kernel::Games::Doom {
void run(const char *wad) {
  char *argv[] = {(char *)"doom",
                  (char *)"-iwad",
                  const_cast<char *>(wad),
                  (char *)"-mb",
                  (char *)"12",
                  (char *)"-savedir",
                  (char *)"/bin/doom/saves"};
  doomgeneric_Create(sizeof(argv) / sizeof(argv[0]), argv);
  bool running = true;
  while (running && !Kernel::Scheduler::should_stop()) {
    doomgeneric_Tick();
  }
}
} // namespace Kernel::Games::Doom
