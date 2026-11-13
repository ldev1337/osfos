#include <arch/x86_64/string.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/fs/laziofs.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/appcommon.h>
#include <kernel/lib/uicommon.h>
#include <kernel/scheduler.h>

namespace Kernel::Apps {
namespace {
using Kernel::FS::VFS;
using Kernel::FS::VFSFile;
using Kernel::Graphics::VirtualFramebuffer;
using namespace Kernel::UICommon;

constexpr uint32_t BG = 0xFF171D24;
constexpr uint32_t PANEL = 0xFF232C35;
constexpr uint32_t ROW = 0xFF1D252D;
constexpr uint32_t SELECTED = 0xFF294248;
constexpr uint32_t TEXT = 0xFFE7ECEF;
constexpr uint32_t MUTED = 0xFF9AAAB1;
constexpr uint32_t ACCENT = 0xFF58C4B8;
constexpr uint32_t ERROR = 0xFFE58F83;
constexpr uint32_t MAX_VIEW_BYTES = 3072;
constexpr int32_t LIST_ROW_HEIGHT = 22;

bool find_next(uint32_t from, bool forward, VFSFile *file) {
  if (forward) {
    for (uint32_t index = from; index < Kernel::FS::VFS_MAX_INODES; ++index) {
      if (VFS::get_file_at(index, file) && file->exists && !file->is_directory)
        return true;
    }
  } else {
    uint32_t index = from;
    while (index > 0) {
      --index;
      if (VFS::get_file_at(index, file) && file->exists && !file->is_directory)
        return true;
    }
  }
  return false;
}

bool move_selection(bool forward, uint32_t steps, uint32_t &selected_index,
                    uint32_t &first_index, int32_t visible_rows,
                    VFSFile &selected_file) {
  bool moved = false;
  uint32_t search = forward ? selected_index + 1 : selected_index;
  VFSFile candidate{};
  for (uint32_t step = 0; step < steps; ++step) {
    if (!find_next(search, forward, &candidate))
      break;
    selected_file = candidate;
    selected_index = candidate.entry_index;
    search = forward ? selected_index + 1 : selected_index;
    moved = true;
  }
  if (moved) {
    if (selected_index < first_index)
      first_index = selected_index;
    else if (selected_index >= first_index + static_cast<uint32_t>(visible_rows))
      first_index = selected_index;
  }
  return moved;
}

void draw_size(VirtualFramebuffer &vfb, int32_t x, int32_t y, uint32_t size,
               uint32_t color, float font_size) {
  char digits[11];
  char text[11];
  size_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + size % 10);
    size /= 10;
  } while (size && count < sizeof(digits));
  for (size_t i = 0; i < count; ++i)
    text[i] = digits[count - i - 1];
  text[count] = '\0';
  Kernel::UICommon::text(vfb, x, y, text, color, font_size);
}

void draw_file_rows(VirtualFramebuffer &vfb, uint32_t selected,
                    uint32_t first_index, int32_t list_x, int32_t list_y,
                    int32_t list_width, int32_t visible_rows) {
  uint32_t index = first_index;
  int32_t row = 0;
  while (index < Kernel::FS::VFS_MAX_INODES && row < visible_rows) {
    VFSFile file{};
    if (VFS::get_file_at(index, &file) && file.exists && !file.is_directory) {
      const int32_t y = list_y + row * LIST_ROW_HEIGHT;
      vfb.fill_rect(list_x, y, list_width, LIST_ROW_HEIGHT - 1,
                    index == selected ? SELECTED : ROW);
      char display_name[Kernel::FS::VFS_MAX_PATH];
      strncpy(display_name, file.name, sizeof(display_name) - 1);
      display_name[sizeof(display_name) - 1] = '\0';
      size_t name_length = strlen(display_name);
      while (name_length > 3 &&
             Kernel::UICommon::text_width(display_name, 12.0f) >
                 list_width - 82) {
        name_length -= 1;
        display_name[name_length] = '\0';
      }
      if (name_length < strlen(file.name) && name_length >= 3) {
        display_name[name_length - 3] = '.';
        display_name[name_length - 2] = '.';
        display_name[name_length - 1] = '.';
      }
      Kernel::UICommon::text(vfb, list_x + 8, y + 4, display_name, TEXT,
                              12.0f);
      char size_text[11];
      uint32_t remaining_size = file.size;
      size_t size_length = 0;
      do {
        size_text[size_length++] = static_cast<char>('0' + remaining_size % 10);
        remaining_size /= 10;
      } while (remaining_size && size_length < sizeof(size_text) - 1);
      size_text[size_length] = '\0';
      for (size_t i = 0; i < size_length / 2; ++i) {
        const char temp = size_text[i];
        size_text[i] = size_text[size_length - i - 1];
        size_text[size_length - i - 1] = temp;
      }
      const int size_width = Kernel::UICommon::text_width(size_text, 11.0f);
      Kernel::UICommon::text(vfb, list_x + list_width - size_width - 8,
                              y + 5, size_text, MUTED, 11.0f);
      ++row;
    }
    ++index;
  }
  while (row < visible_rows) {
    const int32_t y = list_y + row * LIST_ROW_HEIGHT;
    vfb.fill_rect(list_x, y, list_width, LIST_ROW_HEIGHT - 1, BG);
    ++row;
  }
}

} // namespace

void file_viewer_run() {
  if (!VFS::is_mounted())
    return;
  VirtualFramebuffer vfb;
  if (!vfb.open_window(820, 560, 540, 360))
    return;

  Label title(16, 10, "FILE VIEWER", TEXT);
  title.set_font_size_pixels(18.0f);
  Button open_button(16, 40, 76, 28, "Open", ACCENT, BG, 0);
  Button back_button(100, 40, 76, 28, "Close");
  Button up_button(188, 40, 42, 28, "Up");
  Button down_button(236, 40, 58, 28, "Down");
  Button page_up_button(302, 40, 72, 28, "Page -");
  Button page_down_button(382, 40, 72, 28, "Page +");
  open_button.set_font_size_pixels(12.0f);
  back_button.set_font_size_pixels(12.0f);
  up_button.set_font_size_pixels(12.0f);
  down_button.set_font_size_pixels(12.0f);
  page_up_button.set_font_size_pixels(12.0f);
  page_down_button.set_font_size_pixels(12.0f);
  StatusBar status(12, static_cast<int32_t>(vfb.height()) - 34,
                   static_cast<int32_t>(vfb.width()) - 24, 24,
                   "Select a file, then choose Open.", PANEL, MUTED, 0);
  status.set_font_size_pixels(12.0f);

  TextArea document(330, 108, static_cast<int32_t>(vfb.width()) - 352,
                    static_cast<int32_t>(vfb.height()) - 174, "",
                    0xFF12171C, TEXT, ACCENT);
  document.set_font_size_pixels(13.0f);
  WidgetGroup controls;
  controls.add(open_button);
  controls.add(back_button);
  controls.add(up_button);
  controls.add(down_button);
  controls.add(page_up_button);
  controls.add(page_down_button);

  VFSFile selected_file{};
  bool has_selection = find_next(0, true, &selected_file);
  uint32_t selected_index = has_selection ? selected_file.entry_index : 0;
  uint32_t first_index = 0;
  uint32_t view_offset = 0;
  uint32_t loaded_offset = UINT32_MAX;
  char document_text[MAX_VIEW_BYTES + 1];
  bool viewing = false;
  bool running = true;
  bool previous_left = Kernel::UICommon::primary_pressed();
  const int32_t list_x = 12;
  const int32_t list_y = 82;
  const int32_t list_width = 300;
  const int32_t body_bottom = static_cast<int32_t>(vfb.height()) - 44;
  const int32_t body_height = body_bottom - list_y;
  const int32_t visible_rows = body_height / LIST_ROW_HEIGHT;

  while (running && !Kernel::Scheduler::should_stop()) {
    if (!vfb.has_focus()) {
      Kernel::Scheduler::thread_sleep_ms(16);
      continue;
    }
    bool changed = false;
    const int mx = Kernel::UICommon::mouse_x();
    const int my = Kernel::UICommon::mouse_y();
    const bool left = Kernel::UICommon::primary_pressed();
    const bool released = previous_left && !left;
    previous_left = left;

    Widget *clicked = controls.handle_mouse(mx, my, left, released);
    const bool open_clicked = clicked == &open_button;
    const bool back_clicked = clicked == &back_button;
    const bool up_clicked = clicked == &up_button;
    const bool down_clicked = clicked == &down_button;
    const bool page_up_clicked = clicked == &page_up_button;
    const bool page_down_clicked = clicked == &page_down_button;
    if (viewing && document.handle_mouse(mx, my, left, released))
      changed = true;

    if (!viewing && released && mx >= list_x && mx < list_x + list_width &&
        my >= list_y && my < body_bottom) {
      const int32_t clicked_row = (my - list_y) / LIST_ROW_HEIGHT;
      uint32_t index = first_index;
      int32_t row = 0;
      while (index < Kernel::FS::VFS_MAX_INODES && row <= clicked_row) {
        VFSFile item{};
        if (VFS::get_file_at(index, &item) && item.exists &&
            !item.is_directory) {
          if (row == clicked_row) {
            selected_file = item;
            selected_index = index;
            has_selection = true;
            changed = true;
            break;
          }
          ++row;
        }
        ++index;
      }
    }

    if (open_clicked && has_selection) {
      viewing = true;
      document.set_focused(true);
      view_offset = 0;
      changed = true;
    }
    if (back_clicked) {
      if (viewing) {
        viewing = false;
        document.set_focused(false);
        changed = true;
      } else {
        running = false;
      }
    }
    if (up_clicked) {
      if (viewing) {
        changed |= document.handle_key('k');
      } else {
        changed |= move_selection(false, 1, selected_index, first_index,
                visible_rows, selected_file);
      }
    }
    if (down_clicked) {
      if (viewing) {
        changed |= document.handle_key('j');
      } else {
        changed |= move_selection(true, 1, selected_index, first_index,
                visible_rows, selected_file);
      }
    }
    if (page_up_clicked) {
      if (viewing) {
        view_offset = view_offset > MAX_VIEW_BYTES
                          ? view_offset - MAX_VIEW_BYTES
                          : 0;
        loaded_offset = UINT32_MAX;
        changed = true;
      } else {
        changed |= move_selection(false, visible_rows, selected_index,
                first_index, visible_rows, selected_file);
      }
    }
    if (page_down_clicked) {
      if (viewing) {
        view_offset = view_offset + MAX_VIEW_BYTES < selected_file.size
                          ? view_offset + MAX_VIEW_BYTES
                          : (selected_file.size > MAX_VIEW_BYTES
                                 ? selected_file.size - MAX_VIEW_BYTES
                                 : 0);
        loaded_offset = UINT32_MAX;
        changed = true;
      } else {
        changed |= move_selection(true, visible_rows, selected_index,
                first_index, visible_rows, selected_file);
      }
    }

    while (Kernel::Drivers::Keyboard::available()) {
      const char key = Kernel::Drivers::Keyboard::getchar();
      if (key == 27 || key == 'q' || key == 'Q') {
        if (viewing) {
          viewing = false;
          document.set_focused(false);
          changed = true;
        } else {
          running = false;
        }
      } else if (viewing) {
        if (key == 'b' || key == 'B') {
          viewing = false;
          document.set_focused(false);
          changed = true;
        } else if (key == 'k' || key == 'K') {
          changed |= document.handle_key('k');
        } else if (key == 'j' || key == 'J') {
          changed |= document.handle_key('j');
        } else if (key == ' ') {
          view_offset = view_offset + MAX_VIEW_BYTES < selected_file.size
                            ? view_offset + MAX_VIEW_BYTES
                            : (selected_file.size > MAX_VIEW_BYTES
                                   ? selected_file.size - MAX_VIEW_BYTES
                                   : 0);
          loaded_offset = UINT32_MAX;
          changed = true;
        }
      } else if (key == '\n' || key == '\r') {
        if (has_selection) {
          viewing = true;
          view_offset = 0;
          changed = true;
        }
      } else if (key == 'j' || key == 'J' || key == 's' || key == 'S') {
        changed |= move_selection(true, 1, selected_index, first_index,
                visible_rows, selected_file);
      } else if (key == 'k' || key == 'K' || key == 'w' || key == 'W') {
        changed |= move_selection(false, 1, selected_index, first_index,
                visible_rows, selected_file);
      }
    }

    if (changed || title.is_dirty() || open_button.is_dirty() ||
        back_button.is_dirty() || up_button.is_dirty() ||
        down_button.is_dirty() || page_up_button.is_dirty() ||
        page_down_button.is_dirty() || status.is_dirty()) {
      vfb.clear(BG);
      vfb.fill_rect(0, 0, vfb.width(), 36, PANEL);
      title.draw(vfb);
      controls.draw(vfb, false);
      vfb.fill_rect(10, 76, 304, body_height + 8, PANEL);
      vfb.fill_rect(322, 76, vfb.width() - 334, body_height + 8, PANEL);
      if (viewing) {
        if (loaded_offset != view_offset) {
          const uint32_t remaining = view_offset < selected_file.size
                                         ? selected_file.size - view_offset
                                         : 0;
          const uint32_t read_length = remaining < MAX_VIEW_BYTES
                                           ? remaining
                                           : MAX_VIEW_BYTES;
          int bytes_read = VFS::read_file(&selected_file, document_text,
                                          read_length, view_offset);
          if (bytes_read < 0)
            bytes_read = 0;
          for (int i = 0; i < bytes_read; ++i) {
            const unsigned char value =
                static_cast<unsigned char>(document_text[i]);
            if (value != '\n' && value != '\r' &&
                (value < 32 || value >= 127))
              document_text[i] = '.';
          }
          document_text[bytes_read] = '\0';
          document.set_text(document_text);
          document.set_scroll_line(0);
          loaded_offset = view_offset;
        }
        Kernel::UICommon::text(vfb, 24, 86, selected_file.name, ACCENT,
                                14.0f);
        document.draw(vfb);
        status.set_status("j/k scroll  Page +/- browse file  Esc back");
      } else {
        Kernel::UICommon::text(vfb, 22, 86, "FILES", ACCENT, 13.0f);
        draw_file_rows(vfb, selected_index, first_index, list_x, list_y,
                       list_width, visible_rows);
        if (has_selection) {
          Kernel::UICommon::text(vfb, 338, 100, selected_file.name, TEXT,
                                  14.0f);
          Kernel::UICommon::text(vfb, 338, 130, "Size", MUTED, 12.0f);
          draw_size(vfb, 390, 130, selected_file.size, TEXT, 12.0f);
          Kernel::UICommon::text(vfb, 338, 162,
                                  "Press Enter or Open to view", MUTED, 12.0f);
        } else {
          Kernel::UICommon::text(vfb, 338, 100, "No files found.", MUTED,
                                  14.0f);
        }
        status.set_status("Up/Down select  Enter open  Esc quit");
      }
      status.draw(vfb);
      vfb.flush();
      title.clear_dirty();
      open_button.clear_dirty();
      back_button.clear_dirty();
      up_button.clear_dirty();
      down_button.clear_dirty();
      page_up_button.clear_dirty();
      page_down_button.clear_dirty();
      status.clear_dirty();
    }
    Kernel::Scheduler::thread_sleep_ms(16);
  }
}

} // namespace Kernel::Apps
