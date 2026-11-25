#ifndef SCHED_TRACE
#define SCHED_TRACE 0
#endif
#if SCHED_TRACE
#include <kernel/debug/debug.h>
#endif
#include <kernel/drivers/tsc_deadline.h>
#include <kernel/graphics/lwm.h>
#include <kernel/memory/heap.h>
#include <kernel/mtask/internal.h>
#include <stdlib.h>
#include <string.h>

namespace Kernel::Scheduler {

#define SCHED_LIKELY(x) __builtin_expect(!!(x), 1)
#define SCHED_UNLIKELY(x) __builtin_expect(!!(x), 0)

static_assert(offsetof(CpuSchedulerState, current) == 16);
static inline ThreadControlBlock *current_thread_fast() {
  ThreadControlBlock *thread;
  __asm__ volatile("movq %%gs:16, %0" : "=r"(thread));
  return thread;
}

static inline void unlink_thread(ThreadControlBlock *thread) {
  ThreadControlBlock *const prev = thread->registry_prev;
  ThreadControlBlock *const next = thread->registry_next;
  if (prev)
    prev->registry_next = next;
  else
    thread_list_head = next;
  if (next)
    next->registry_prev = prev;
  thread->registry_prev = nullptr;
  thread->registry_next = nullptr;
}

void reap_deferred() {
  const uint64_t flags = irq_save();
  ThreadControlBlock *pending = reap_pending;
  if (!pending) {
    irq_restore(flags);
    return;
  }
  reap_pending = nullptr;
  for (ThreadControlBlock *thread = pending; thread; thread = thread->reap_next)
    unlink_thread(thread);
  irq_restore(flags);

  while (pending) {
    ThreadControlBlock *const thread = pending;
    pending = thread->reap_next;
    if (thread->stack_mem)
      free(thread->stack_mem);
    free(thread);
  }
}

extern "C" void thread_start_wrapper(void (*entry)()) {
  entry();
  thread_exit();
}

extern "C" void thread_start_arg_wrapper(ThreadEntry entry, void *argument) {
  entry(argument);
  thread_exit();
}

static auto create_thread(void (*entry)(), ThreadEntry arg_entry,
                          void *argument, ThreadPriority priority) -> int {
  if (SCHED_UNLIKELY(!scheduler_initialized || (!entry && !arg_entry)))
    return -1;

  reap_deferred();

  auto *thread =
      static_cast<ThreadControlBlock *>(malloc(sizeof(ThreadControlBlock)));
  if (SCHED_UNLIKELY(!thread))
    return -1;

  auto *stack_mem = static_cast<uint64_t *>(malloc(THREAD_STACK_SIZE));
  if (SCHED_UNLIKELY(!stack_mem)) {
    free(thread);
    return -1;
  }

  memset(thread, 0, sizeof(*thread));
  if (SCHED_UNLIKELY(!initialize_context(thread, stack_mem, THREAD_STACK_SIZE,
                                         entry, arg_entry, argument))) {
    free(stack_mem);
    free(thread);
    return -1;
  }

  thread->priority = static_cast<ThreadPriority>(priority_index(priority));
  thread->stop_requested = false;
  thread->wake_tick = 0;
  thread->timed_wait = false;
  thread->sleep_next = nullptr;

  const uint64_t flags = irq_save();
  const uint64_t id = next_pid++;
  thread->id = id;
  register_thread(thread);
  enqueue(thread);
  irq_restore(flags);

#if SCHED_TRACE
  Kernel::Debug::log("task added to scheduler");
  Kernel::Debug::log_dec("task_id", id);
  Kernel::Debug::log_dec("task_priority", priority_index(priority));
#endif
  return static_cast<int>(id);
}

auto thread_create(void (*entry)(), ThreadPriority priority) -> int {
  return create_thread(entry, nullptr, nullptr, priority);
}

auto thread_create(ThreadEntry entry, void *argument, ThreadPriority priority)
    -> int {
  return create_thread(nullptr, entry, argument, priority);
}

auto current_thread_id() -> uint64_t {
  ThreadControlBlock *const current = current_thread_fast();
  return current ? current->id : 0;
}

void mark_current_thread_as_application(const char *name) {
  constexpr size_t NAME_CAP =
      sizeof(((ThreadControlBlock *)nullptr)->application_name);
  char name_copy[NAME_CAP]{};
  if (name) {
    size_t i = 0;
    for (; i + 1 < NAME_CAP && name[i]; ++i)
      name_copy[i] = name[i];
  }
  const uint64_t start_ticks = Kernel::Drivers::TSCDeadline::get_ticks();

  const uint64_t flags = irq_save();
  ThreadControlBlock *const current = cpu_state().current;
  if (current) {
    current->application_thread = true;
    current->application_start_ticks = start_ticks;
    __builtin_memcpy(current->application_name, name_copy, NAME_CAP);
  }
  irq_restore(flags);
}

bool is_current_thread_application() {
  ThreadControlBlock *const current = current_thread_fast();
  return current && current->application_thread;
}

size_t get_application_tasks(ApplicationTaskInfo *tasks, size_t capacity) {
  if (SCHED_UNLIKELY(!tasks || !capacity))
    return 0;

  size_t count = 0;
  const uint64_t flags = irq_save();
  for (ThreadControlBlock *thread = thread_list_head;
       thread && count < capacity; thread = thread->registry_next) {
    if (SCHED_UNLIKELY(!thread->application_thread ||
                       thread->state == ThreadState::TERMINATED))
      continue;

    ApplicationTaskInfo &task = tasks[count++];
    task.id = thread->id;
    task.start_ticks = thread->application_start_ticks;
    size_t i = 0;
    for (; i + 1 < sizeof(task.name) && thread->application_name[i]; ++i)
      task.name[i] = thread->application_name[i];
    task.name[i] = '\0';
  }
  irq_restore(flags);
  return count;
}

void terminate_thread(uint64_t id) {
  if (SCHED_UNLIKELY(!id))
    return;

  const uint64_t flags = irq_save();
  for (ThreadControlBlock *thread = thread_list_head; thread;
       thread = thread->registry_next) {
    if (thread->id != id || thread->state == ThreadState::TERMINATED)
      continue;

    // The target may be running on another CPU. Pair this store with the
    // relaxed atomic load in should_stop(); no scheduler lock is needed there.
    __atomic_store_n(&thread->stop_requested, true, __ATOMIC_RELAXED);
    if (thread->state == ThreadState::BLOCKED) {
      if (thread->timed_wait)
        remove_sleeper(thread);
      else
        remove_blocked(thread);
      thread->timed_wait = false;
      enqueue(thread);
    }
    irq_restore(flags);
#if SCHED_TRACE
    Kernel::Debug::log("task termination requested");
    Kernel::Debug::log_dec("task_id", id);
#endif
    return;
  }
  irq_restore(flags);
#if SCHED_TRACE
  Kernel::Debug::log("task termination request ignored: task not found");
  Kernel::Debug::log_dec("task_id", id);
#endif
}

auto should_stop() -> bool {
  ThreadControlBlock *const current = current_thread_fast();
  return current && __atomic_load_n(&current->stop_requested, __ATOMIC_RELAXED);
}

[[noreturn]] void thread_exit() {
  CpuSchedulerState &state = cpu_state();
  ThreadControlBlock *const current = state.current;
  if (current && current != state.idle) {
    Kernel::Graphics::WindowManager::unregister_window(current->id);
  }

  (void)irq_save();
  if (current && current != state.idle) {
    current->state = ThreadState::TERMINATED;
    current->timed_wait = false;
  }
  yield_holding_lock(state);
  for (;;)
    __asm__ volatile("hlt" ::: "memory");
}

#undef SCHED_LIKELY
#undef SCHED_UNLIKELY

} // namespace Kernel::Scheduler

extern "C" [[noreturn]] void exit(int status) {
  (void)status;
  Kernel::Scheduler::thread_exit();
}
