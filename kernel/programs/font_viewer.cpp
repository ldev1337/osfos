#include <string.h>
#include <oslib/keyboard.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/open_font.h>
#include <oslib/win.h>
#include <oslib/ui.h>
#include <kernel/lib/appcommon.h>
#include <kernel/scheduler.h>

namespace Kernel::Apps {
namespace {
using Kernel::AppCommon::delay;
using oslib::Keyboard;

constexpr uint32_t BG = 0x121B2B;
constexpr uint32_t PANEL = 0x1F2A3A;
constexpr uint32_t FG = 0xF0F6FF;
constexpr uint32_t MUTED = 0xA7B7D1;
constexpr uint32_t GREEN = 0x7FE7A1;
constexpr uint32_t BLUE = 0x8BC2FF;
constexpr uint32_t RED = 0xFF8B8B;
constexpr uint32_t GOLD = 0xF3C77A;
constexpr size_t MAX_FONT_CHOICES = 16;

void draw_status(oslib::Win &win, int32_t x,
                 int32_t y, const char *label, const char *status,
                 bool ok) {
  const uint32_t box = ok ? 0x1B3C2B : 0x3B2525;
  const uint32_t text = ok ? GREEN : RED;
  win.fill_rect(x, y, 260, 22, box);
  win.draw_rect(x, y, 260, 22, PANEL);
  UI::text(win, x + 8, y + 6, label, FG);
  UI::text(win, x + 150, y + 6, status, text);
}

void draw_selection_bar(oslib::Win &win,
                        int32_t x, int32_t y, const char *font_name,
                        bool selected, bool available) {
  const uint32_t bg = selected ? 0x27354C : 0x1A2430;
  const uint32_t text = available ? FG : MUTED;
  win.fill_rect(x, y, 320, 22, bg);
  win.draw_rect(x, y, 320, 22, PANEL);
  UI::text(win, x + 8, y + 6, selected ? ">" : " ", GOLD);
  UI::text(win, x + 24, y + 6, font_name, text);
}

bool load_font_choice(Kernel::Graphics::TrueTypeFont &font, const char *font_name) {
  if (!font_name || font_name[0] == '\0')
    return Kernel::Graphics::OpenFont::load_embedded(font);
  return Kernel::Graphics::OpenFont::load(font, font_name);
}

} // namespace

void run_font_viewer() {
  oslib::Win win;
  if (!win.open(760, 470, 320, 260))
    return;

  Kernel::Graphics::TrueTypeFont embedded_font;
  const bool embedded_ok = Kernel::Graphics::OpenFont::load_embedded(embedded_font);

  char font_names[MAX_FONT_CHOICES][96]{};
  size_t font_count = 0;
  if (Kernel::FS::VFS::is_mounted())
    font_count = Kernel::Graphics::OpenFont::enumerate_available(&font_names[0][0], MAX_FONT_CHOICES, sizeof(font_names[0]));

  size_t selected_index = 0;
  Kernel::Graphics::TrueTypeFont active_font;
  char active_name[96]{};
  bool active_loaded = false;

  if (font_count > 0) {
    selected_index = 0;
    active_loaded = load_font_choice(active_font, font_names[selected_index]);
    strncpy(active_name, font_names[selected_index], sizeof(active_name) - 1);
    active_name[sizeof(active_name) - 1] = '\0';
  } else {
    active_loaded = embedded_ok;
    strncpy(active_name, "embedded fallback", sizeof(active_name) - 1);
    active_name[sizeof(active_name) - 1] = '\0';
  }

  bool running = true;
  while (running && !Kernel::Scheduler::should_stop()) {
    while (Keyboard::available()) {
      const char key = Keyboard::getchar();
      if (key == 'q' || key == 'Q' || key == 27)
        running = false;
      else if (key == 'n' || key == 'N' || key == 'j' || key == 'J') {
        if (font_count > 1) {
          selected_index = (selected_index + 1) % font_count;
          active_loaded = load_font_choice(active_font, font_names[selected_index]);
          strncpy(active_name, font_names[selected_index], sizeof(active_name) - 1);
          active_name[sizeof(active_name) - 1] = '\0';
        }
      } else if (key == 'p' || key == 'P' || key == 'k' || key == 'K') {
        if (font_count > 1) {
          if (selected_index == 0)
            selected_index = font_count - 1;
          else
            selected_index--;
          active_loaded = load_font_choice(active_font, font_names[selected_index]);
          strncpy(active_name, font_names[selected_index], sizeof(active_name) - 1);
          active_name[sizeof(active_name) - 1] = '\0';
        }
      }
    }

    win.clear(BG);
    win.fill_rect(0, 0, win.width(), 30, PANEL);
    UI::text(win, 16, 8, "FONT VIEWER - AVAILABLE TTFs", FG);

    draw_status(win, 12, 42, "embedded", embedded_ok ? "READY" : "MISSING", embedded_ok);
    int32_t list_y = 78;
    for (size_t i = 0; i < font_count && i < MAX_FONT_CHOICES; ++i) {
      const bool selected = (i == selected_index);
      char label[96];
      strncpy(label, font_names[i], sizeof(label) - 1);
      label[sizeof(label) - 1] = '\0';
      draw_selection_bar(win, 12, list_y, label, selected, true);
      list_y += 24;
    }
    if (font_count == 0) {
      draw_selection_bar(win, 12, list_y, "no .ttf files in fonts/", false, false);
      list_y += 24;
    }

    const int32_t sample_y = 270;
    win.fill_rect(12, sample_y, win.width() - 24, 1, PANEL);
    if (active_loaded) {
      UI::text(win, 18, sample_y + 18, 20.0f, active_font,
                            "The quick brown fox jumps over the lazy dog.", BLUE);
      UI::text(win, 18, sample_y + 52, 16.0f, active_font,
                            "OSFOS font sample text", GOLD);
      UI::text(win, 18, sample_y + 82, active_name, GREEN);
    } else {
      UI::text(win, 18, sample_y + 18, "No valid font selected.", RED);
    }

    UI::text(win, 18, sample_y + 104, 12.0f, embedded_font,
                          "n/p or j/k switch fonts  q quits", MUTED);
    win.flush();
    delay(4);
  }
}

void font_viewer_run() { run_font_viewer(); }

} // namespace Kernel::Apps
