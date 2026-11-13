#pragma once

#include <cstdint>

namespace Kernel::Graphics::WindowManager {

bool initialize();
int register_window(uint64_t thread_id, const char *title,
                    bool launcher = false, uint32_t width = 0,
                    uint32_t height = 0);
bool request_window_size(uint64_t thread_id, uint32_t requested_width,
                         uint32_t requested_height, uint32_t *actual_width,
                         uint32_t *actual_height);
void unregister_window(uint64_t thread_id);
bool present(uint64_t thread_id, const uint32_t *pixels, uint32_t width,
             uint32_t height, uint32_t pitch);
bool has_window(uint64_t thread_id);
bool title_is_open(const char *title);
bool has_keyboard_focus(uint64_t thread_id);
uint64_t keyboard_focus_thread_id();
bool get_window_position(uint64_t thread_id, int *x, int *y);
void run();

} // namespace Kernel::Graphics::WindowManager
