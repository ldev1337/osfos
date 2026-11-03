#include <cstddef>
#include <cstdint>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/pit.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/window_manager.h>
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

void DG_SetWindowTitle(const char * /*unused*/) {}
}

static void show_startup_error(bool disk_not_initialized) {
  if (!doom_window || !doom_window->open_window(640, 400)) {
    return;
  }

  doom_window->fill(0xFF17191D);
  doom_window->fill_rect(24, 72, 4, 56, 0xFFE05A47);

  const char *title = "DOOM COULD NOT START";
  const char *message = disk_not_initialized
                            ? "Disk is not initialized."
                            : "WAD file was not found.";
  int32_t cursor_x = 44;
  for (const char *character = title; *character != '\0';
       ++character, cursor_x += 8) {
    doom_window->draw_glyph_bg(cursor_x, 76, *character, 0xFFFFC857,
                   0xFF17191D);
  }

  cursor_x = 44;
  for (const char *character = message; *character != '\0';
       ++character, cursor_x += 8) {
    doom_window->draw_glyph_bg(cursor_x, 100, *character, 0xFFE8E8E8,
                               0xFF17191D);
  }

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

  void *wad_file = wad != nullptr && wad[0] != '\0'
                       ? kernel_vfs_open(wad, "r")
                       : nullptr;
  if (wad_file == nullptr) {
    show_startup_error(false);
    while (!Kernel::Scheduler::should_stop()) {
      Kernel::Scheduler::thread_sleep_ms(50);
    }
    doom_window = nullptr;
    return;
  }
  kernel_vfs_close(wad_file);

  char *argv[] = {(char *)"doom",
                  (char *)"-iwad",
                  const_cast<char *>(wad),
                  (char *)"-mb",
                  (char *)"12",
                  (char *)"-savedir",
                  (char *)"/bin/doom/saves",};
  doom_quit_requested = false;
  doomgeneric_Create(sizeof(argv) / sizeof(argv[0]), argv);
  bool running = true;
  while (running && !doom_quit_requested &&
         !Kernel::Scheduler::should_stop()) {
    doomgeneric_Tick();
  }
  I_Quit();
  doomgeneric_Shutdown();
  doom_window = nullptr;
}
} // namespace Kernel::Games::Doom
