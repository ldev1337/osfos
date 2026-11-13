#include <arch/x86_64/string.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/open_font.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/uicommon.h>
#include <kernel/lib/appcommon.h>
#include <kernel/scheduler.h>

namespace Kernel::Apps {
namespace {
using Kernel::AppCommon::delay;
using Kernel::Drivers::Keyboard;

constexpr uint32_t BG = 0x121B2B;
constexpr uint32_t PANEL = 0x1F2A3A;
constexpr uint32_t FG = 0xF0F6FF;
constexpr uint32_t MUTED = 0xA7B7D1;
constexpr uint32_t GREEN = 0x7FE7A1;
constexpr uint32_t BLUE = 0x8BC2FF;
constexpr uint32_t RED = 0xFF8B8B;
constexpr uint32_t GOLD = 0xF3C77A;
constexpr size_t MAX_FONT_CHOICES = 16;

void draw_status(Kernel::Graphics::VirtualFramebuffer &vfb, int32_t x,
                 int32_t y, const char *label, const char *status,
                 bool ok) {
  const uint32_t box = ok ? 0x1B3C2B : 0x3B2525;
  const uint32_t text = ok ? GREEN : RED;
  vfb.fill_rect(x, y, 260, 22, box);
  vfb.draw_rect(x, y, 260, 22, PANEL);
  Kernel::UICommon::text(vfb, x + 8, y + 6, label, FG);
  Kernel::UICommon::text(vfb, x + 150, y + 6, status, text);
}

void draw_selection_bar(Kernel::Graphics::VirtualFramebuffer &vfb,
                        int32_t x, int32_t y, const char *font_name,
                        bool selected, bool available) {
  const uint32_t bg = selected ? 0x27354C : 0x1A2430;
  const uint32_t text = available ? FG : MUTED;
  vfb.fill_rect(x, y, 320, 22, bg);
  vfb.draw_rect(x, y, 320, 22, PANEL);
  Kernel::UICommon::text(vfb, x + 8, y + 6, selected ? ">" : " ", GOLD);
  Kernel::UICommon::text(vfb, x + 24, y + 6, font_name, text);
}

bool load_font_choice(Kernel::Graphics::TrueTypeFont &font, const char *font_name) {
  if (!font_name || font_name[0] == '\0')
    return Kernel::Graphics::OpenFont::load_embedded(font);
  return Kernel::Graphics::OpenFont::load(font, font_name);
}

} // namespace

void run_font_viewer() {
  Kernel::Graphics::VirtualFramebuffer vfb;
  if (!vfb.open_window(760, 470, 320, 260))
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

    vfb.clear(BG);
    vfb.fill_rect(0, 0, vfb.width(), 30, PANEL);
    Kernel::UICommon::text(vfb, 16, 8, "FONT VIEWER - AVAILABLE TTFs", FG);

    draw_status(vfb, 12, 42, "embedded", embedded_ok ? "READY" : "MISSING", embedded_ok);
    int32_t list_y = 78;
    for (size_t i = 0; i < font_count && i < MAX_FONT_CHOICES; ++i) {
      const bool selected = (i == selected_index);
      char label[96];
      strncpy(label, font_names[i], sizeof(label) - 1);
      label[sizeof(label) - 1] = '\0';
      draw_selection_bar(vfb, 12, list_y, label, selected, true);
      list_y += 24;
    }
    if (font_count == 0) {
      draw_selection_bar(vfb, 12, list_y, "no .ttf files in fonts/", false, false);
      list_y += 24;
    }

    const int32_t sample_y = 270;
    vfb.fill_rect(12, sample_y, vfb.width() - 24, 1, PANEL);
    if (active_loaded) {
      Kernel::UICommon::text(vfb, 18, sample_y + 18, 20.0f, active_font,
                            "The quick brown fox jumps over the lazy dog.", BLUE);
      Kernel::UICommon::text(vfb, 18, sample_y + 52, 16.0f, active_font,
                            "OSFOS font sample text", GOLD);
      Kernel::UICommon::text(vfb, 18, sample_y + 82, active_name, GREEN);
    } else {
      Kernel::UICommon::text(vfb, 18, sample_y + 18, "No valid font selected.", RED);
    }

    Kernel::UICommon::text(vfb, 18, sample_y + 104, 12.0f, embedded_font,
                          "n/p or j/k switch fonts  q quits", MUTED);
    vfb.flush();
    delay(4);
  }
}

void font_viewer_run() { run_font_viewer(); }

} // namespace Kernel::Apps
