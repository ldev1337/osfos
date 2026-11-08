#include <arch/x86_64/spinlock.h>
#include <cstddef>

struct DestructorEntry {
  void (*destructor)(void *);
  void *arg;
  void *dso_handle;
};

constexpr size_t MAX_DESTRUCTORS = 256;
static DestructorEntry g_destructors[MAX_DESTRUCTORS];
static size_t g_destructor_count = 0;
static Spinlock g_atexit_lock;

extern "C" {
void *__dso_handle = nullptr;

int __cxa_atexit(void (*destructor)(void *), void *arg, void *dso_handle) {
  if (!destructor) {
    return -1;
  }

  g_atexit_lock.lock();
  if (g_destructor_count >= MAX_DESTRUCTORS) {
    g_atexit_lock.unlock();
    return -1;
  }

  g_destructors[g_destructor_count++] =
      DestructorEntry{destructor, arg, dso_handle};
  g_atexit_lock.unlock();
  return 0;
}

void __os_run_static_destructors(void) {
  constexpr size_t snapshot_count = MAX_DESTRUCTORS;
  DestructorEntry snapshot[MAX_DESTRUCTORS];
  size_t active_count = 0;

  g_atexit_lock.lock();
  active_count = g_destructor_count;
  for (size_t i = 0; i < active_count && i < snapshot_count; ++i) {
    snapshot[i] = g_destructors[i];
  }
  g_destructor_count = 0;
  g_atexit_lock.unlock();

  for (size_t i = active_count; i > 0; --i) {
    const auto &entry = snapshot[i - 1];
    if (entry.destructor) {
      entry.destructor(entry.arg);
    }
  }
}
}