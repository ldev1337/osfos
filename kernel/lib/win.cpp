#include <oslib/win.h>
#include <kernel/graphics/lwm.h>
#include <kernel/scheduler.h>

namespace oslib {

bool Win::present_pixels(const uint32_t *pixels, uint32_t width,
                         uint32_t height, uint32_t pitch) const {
  if (!pixels || !width || !height || !pitch)
    return false;
  return Kernel::Graphics::WindowManager::present(
      Kernel::Scheduler::current_thread_id(), pixels, width, height, pitch);
}

bool Win::exists() const {
  return Kernel::Graphics::WindowManager::has_window(
      Kernel::Scheduler::current_thread_id());
}

void Win::unregister() {
  Kernel::Graphics::WindowManager::unregister_window(
      Kernel::Scheduler::current_thread_id());
}

} // namespace oslib
