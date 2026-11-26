#ifndef SCHED_TRACE
#define SCHED_TRACE 0
#endif
#if SCHED_TRACE
#include <kernel/debug/debug.h>
#endif
#include <kernel/drivers/tsc_deadline.h>
#include <kernel/graphics/lwm.h>
#include <kernel/programs/terminal.h>
#include <kernel/mtask/internal.h>
#include <kernel/memory/heap.h>
#include <stdlib.h>
#include <string.h>

extern "C" void os_stdio_process_cleanup(void *context);

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

static void free_process_arguments(ProcessControlBlock *process) {
  if (process && process->argv) {
    free(process->argv);
    process->argv = nullptr;
  }
}

static bool copy_process_arguments(ProcessControlBlock *process, int argc,
                                   const char *const argv[]) {
  if (!process || argc < 0 || argc > 32 || (argc && !argv))
    return false;
  size_t pointer_bytes = (static_cast<size_t>(argc) + 1) * sizeof(char *);
  size_t total = pointer_bytes;
  for (int i = 0; i < argc; ++i) {
    if (!argv[i])
      return false;
    const size_t length = strlen(argv[i]) + 1;
    if (length > 1024 || total > static_cast<size_t>(-1) - length)
      return false;
    total += length;
  }
  auto **copy = static_cast<char **>(Kernel::Memory::g_kheap.kmalloc_owned(total, 0));
  if (!copy)
    return false;
  char *cursor = reinterpret_cast<char *>(copy) + pointer_bytes;
  for (int i = 0; i < argc; ++i) {
    const size_t length = strlen(argv[i]) + 1;
    copy[i] = cursor;
    memcpy(cursor, argv[i], length);
    cursor += length;
  }
  copy[argc] = nullptr;
  process->argc = argc;
  process->argv = copy;
  return true;
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

static ThreadControlBlock *allocate_thread(void (*entry)(), ThreadEntry arg_entry,
                                           void *argument,
                                           ThreadPriority priority) {
  auto *thread = static_cast<ThreadControlBlock *>(
      Kernel::Memory::g_kheap.kmalloc_owned(sizeof(ThreadControlBlock), 0));
  if (!thread)
    return nullptr;
  auto *stack_mem = static_cast<uint64_t *>(
      Kernel::Memory::g_kheap.kmalloc_owned(THREAD_STACK_SIZE, 0));
  if (!stack_mem) {
    free(thread);
    return nullptr;
  }
  memset(thread, 0, sizeof(*thread));
  if (!initialize_context(thread, stack_mem, THREAD_STACK_SIZE, entry,
                          arg_entry, argument)) {
    free(stack_mem);
    free(thread);
    return nullptr;
  }
  thread->priority = static_cast<ThreadPriority>(priority_index(priority));
  thread->stop_requested = false;
  thread->wake_tick = 0;
  thread->timed_wait = false;
  thread->sleep_next = nullptr;
  return thread;
}

static auto create_thread(void (*entry)(), ThreadEntry arg_entry,
                          void *argument, ThreadPriority priority) -> int {
  if (SCHED_UNLIKELY(!scheduler_initialized || (!entry && !arg_entry)))
    return -1;
  reap_deferred();
  ThreadControlBlock *thread = allocate_thread(entry, arg_entry, argument,
                                               priority);
  if (!thread)
    return -1;

  const uint64_t flags = irq_save();
  ThreadControlBlock *current = cpu_state().current;
  ProcessControlBlock *process = current ? current->process : kernel_process;
  if (next_tid > 0x7fffffffull || !process || process->exit_requested ||
      __atomic_load_n(&process->state, __ATOMIC_ACQUIRE) !=
          static_cast<uint32_t>(ProcessState::RUNNING)) {
    irq_restore(flags);
    free(thread->stack_mem);
    free(thread);
    return -1;
  }
  thread->id = next_tid++;
  thread->process = process;
  ++process->active_threads;
  register_thread(thread);
  enqueue(thread);
  const uint64_t id = thread->id;
  irq_restore(flags);
#if SCHED_TRACE
  Kernel::Debug::log("thread added to process");
  Kernel::Debug::log_dec("thread_id", id);
#endif
  return static_cast<int>(id);
}

static void process_start_entry(void *argument) {
  auto *process = static_cast<ProcessControlBlock *>(argument);
  const int status = process && process->entry
                         ? process->entry(process->argc, process->argv,
                                          process->entry_context)
                         : -1;
  process_exit(status);
}

auto thread_create(void (*entry)(), ThreadPriority priority) -> int {
  return create_thread(entry, nullptr, nullptr, priority);
}

auto thread_create(ThreadEntry entry, void *argument, ThreadPriority priority)
    -> int {
  return create_thread(nullptr, entry, argument, priority);
}

uint64_t process_spawn(ProcessEntry entry, int argc, const char *const argv[],
                  void *context, const char *name, ThreadPriority priority,
                  uint64_t *main_thread_id, bool auto_reap) {
  if (!scheduler_initialized || !entry || argc < 0 || argc > 32)
    return 0;
  reap_deferred();
  auto *process = static_cast<ProcessControlBlock *>(
      Kernel::Memory::g_kheap.kmalloc_owned(sizeof(ProcessControlBlock), 0));
  if (!process)
    return 0;
  memset(process, 0, sizeof(*process));
  process->state = static_cast<uint32_t>(ProcessState::RUNNING);
  process->entry = entry;
  process->entry_context = context;
  if (!copy_process_arguments(process, argc, argv)) {
    free_process_arguments(process);
    free(process);
    return 0;
  }
  if (name) {
    size_t i = 0;
    for (; i + 1 < sizeof(process->name) && name[i]; ++i)
      process->name[i] = name[i];
    process->name[i] = '\0';
  }

  ThreadControlBlock *thread =
      allocate_thread(nullptr, process_start_entry, process, priority);
  if (!thread) {
    free_process_arguments(process);
    free(process);
    return 0;
  }

  const uint64_t flags = irq_save();
  ThreadControlBlock *current = cpu_state().current;
  ProcessControlBlock *parent = current ? current->process : kernel_process;
  if (next_pid > 0x7fffffffull || next_tid > 0x7fffffffull || !parent ||
      parent->exit_requested ||
      __atomic_load_n(&parent->state, __ATOMIC_ACQUIRE) !=
          static_cast<uint32_t>(ProcessState::RUNNING)) {
    irq_restore(flags);
    free(thread->stack_mem);
    free(thread);
    free_process_arguments(process);
    free(process);
    return 0;
  }

  process->id = next_pid++;
  process->parent = parent;
  process->auto_reap = auto_reap;
  process->terminal_thread_id =
      __atomic_load_n(&parent->terminal_thread_id, __ATOMIC_ACQUIRE);
  process->active_threads = 1;
  thread->id = next_tid++;
  thread->process = process;
  process->main_thread_id = thread->id;
  process->sibling_next = parent->children;
  parent->children = process;
  register_thread(thread);
  enqueue(thread);
  const uint64_t pid = process->id;
  if (main_thread_id)
    *main_thread_id = thread->id;
  irq_restore(flags);
  return pid;
}

int process_wait(uint64_t pid, int *exit_status) {
  if (!pid)
    return -1;
  ThreadControlBlock *current = current_thread_fast();
  ProcessControlBlock *parent = current ? current->process : nullptr;
  if (!parent)
    return -1;

  const uint64_t caller_tid = current->id;
  ProcessControlBlock *child = nullptr;
  uint64_t flags = irq_save();
  for (ProcessControlBlock *candidate = parent->children; candidate;
       candidate = candidate->sibling_next) {
    if (candidate->id == pid) {
      child = candidate;
      break;
    }
  }
  if (!child || (child->waiter_thread_id &&
                 child->waiter_thread_id != caller_tid)) {
    irq_restore(flags);
    return -1;
  }
  child->waiter_thread_id = caller_tid;
  child->auto_reap = false;
  irq_restore(flags);

  while (__atomic_load_n(&child->state, __ATOMIC_ACQUIRE) !=
         static_cast<uint32_t>(ProcessState::ZOMBIE)) {
    thread_block_on(&child->state,
                    static_cast<uint32_t>(ProcessState::RUNNING));
    if (should_stop()) {
      flags = irq_save();
      if (child->waiter_thread_id == caller_tid)
        child->waiter_thread_id = 0;
      irq_restore(flags);
      return -1;
    }
  }

  flags = irq_save();
  ProcessControlBlock **link = &parent->children;
  while (*link && *link != child)
    link = &(*link)->sibling_next;
  if (*link != child) {
    irq_restore(flags);
    return -1;
  }
  *link = child->sibling_next;
  const int status = child->exit_status;
  irq_restore(flags);

  if (exit_status)
    *exit_status = status;
  free_process_arguments(child);
  free(child);
  return 0;
}

uint64_t current_thread_id() {
  ThreadControlBlock *const current = current_thread_fast();
  return current ? current->id : 0;
}

int *current_errno_location() {
  ThreadControlBlock *const current = current_thread_fast();
  return current ? &current->errno_value : nullptr;
}

uint64_t current_process_id() {
  ThreadControlBlock *const current = current_thread_fast();
  return current && current->process ? current->process->id : 0;
}

uint64_t current_parent_process_id() {
  const uint64_t flags = irq_save();
  ThreadControlBlock *const current = cpu_state().current;
  const uint64_t parent_id =
      current && current->process && current->process->parent
          ? current->process->parent->id
          : 0;
  irq_restore(flags);
  return parent_id;
}

bool current_process_is_kernel() {
  ThreadControlBlock *const current = current_thread_fast();
  return current && current->process && current->process == kernel_process;
}

void process_set_terminal_thread_id(uint64_t thread_id) {
  ThreadControlBlock *const current = current_thread_fast();
  if (current && current->process)
    __atomic_store_n(&current->process->terminal_thread_id, thread_id,
                     __ATOMIC_RELEASE);
}

void process_clear_terminal_thread_id(uint64_t thread_id) {
  ThreadControlBlock *const current = current_thread_fast();
  if (!current || !current->process)
    return;
  uint64_t expected = thread_id;
  (void)__atomic_compare_exchange_n(&current->process->terminal_thread_id,
                                    &expected, 0, false, __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED);
}

uint64_t process_terminal_thread_id() {
  ThreadControlBlock *const current = current_thread_fast();
  return current && current->process
             ? __atomic_load_n(&current->process->terminal_thread_id,
                               __ATOMIC_ACQUIRE)
             : 0;
}

void *process_stdio_context() {
  ThreadControlBlock *const current = current_thread_fast();
  return current && current->process
             ? __atomic_load_n(&current->process->stdio_context,
                               __ATOMIC_ACQUIRE)
             : nullptr;
}

void process_set_stdio_context(void *context) {
  ThreadControlBlock *const current = current_thread_fast();
  if (current && current->process)
    __atomic_store_n(&current->process->stdio_context, context,
                     __ATOMIC_RELEASE);
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
    return;
  }
  irq_restore(flags);
}

bool should_stop() {
  ThreadControlBlock *const current = current_thread_fast();
  return current && __atomic_load_n(&current->stop_requested, __ATOMIC_RELAXED);
}

[[noreturn]] void process_exit(int status) {
  ThreadControlBlock *current = current_thread_fast();
  ProcessControlBlock *process = current ? current->process : nullptr;
  if (!process || process == kernel_process)
    thread_exit();

  const uint64_t flags = irq_save();
  if (!process->exit_requested) {
    process->exit_requested = true;
    process->exit_status = status;
  }
  for (ThreadControlBlock *thread = thread_list_head; thread;
       thread = thread->registry_next) {
    if (thread == current || thread->process != process ||
        thread->state == ThreadState::TERMINATED)
      continue;
    __atomic_store_n(&thread->stop_requested, true, __ATOMIC_RELAXED);
    if (thread->state == ThreadState::BLOCKED) {
      if (thread->timed_wait)
        remove_sleeper(thread);
      else
        remove_blocked(thread);
      thread->timed_wait = false;
      enqueue(thread);
    }
  }
  irq_restore(flags);
  thread_exit();
}

[[noreturn]] void thread_exit() {
  CpuSchedulerState &state = cpu_state();
  ThreadControlBlock *const current = state.current;
  if (!current || current == state.idle) {
    for (;;)
      __asm__ volatile("cli; hlt" ::: "memory");
  }

  ProcessControlBlock *process = current->process;
  const uint64_t exiting_tid = current->id;
  Kernel::Programs::Terminal::shutdown();
  process_clear_terminal_thread_id(exiting_tid);
  Kernel::Graphics::WindowManager::unregister_window(exiting_tid);
  Kernel::Memory::g_kheap.free_thread_allocations(exiting_tid);

  void *stdio_context = nullptr;
  bool last_process_thread = false;
  uint64_t flags = irq_save();
  if (process && process->active_threads) {
    --process->active_threads;
    if (process != kernel_process && process->active_threads == 0) {
      last_process_thread = true;
      stdio_context = process->stdio_context;
      process->stdio_context = nullptr;
    }
  }
  if (!last_process_thread) {
    current->process = nullptr;
    current->state = ThreadState::TERMINATED;
    current->timed_wait = false;
    yield_holding_lock(state);
    local_irq_restore(flags);
    for (;;)
      __asm__ volatile("hlt" ::: "memory");
  }
  irq_restore(flags);

  if (stdio_context)
    os_stdio_process_cleanup(stdio_context);
  Kernel::Memory::g_kheap.free_process_allocations(process->id);

  // No live thread retains this PCB now. Clear the TCB link before publishing
  // ZOMBIE so a concurrent waiter/reaper can safely release the process object.
  current->process = nullptr;

  ProcessControlBlock *orphan_zombies = nullptr;
  uint64_t waiter_tid = 0;
  bool reap_self = false;
  flags = irq_save();
  __atomic_store_n(&process->state,
                   static_cast<uint32_t>(ProcessState::ZOMBIE),
                   __ATOMIC_RELEASE);
  ProcessControlBlock *child = process->children;
  process->children = nullptr;
  while (child) {
    ProcessControlBlock *next = child->sibling_next;
    // The last thread of the parent is gone, so any waiter in that process
    // is gone too. Do not leave orphaned children waiting on a dead TID.
    child->waiter_thread_id = 0;
    if (__atomic_load_n(&child->state, __ATOMIC_ACQUIRE) ==
            static_cast<uint32_t>(ProcessState::ZOMBIE)) {
      child->sibling_next = orphan_zombies;
      orphan_zombies = child;
    } else {
      child->parent = kernel_process;
      child->auto_reap = child->waiter_thread_id == 0;
      child->sibling_next = kernel_process ? kernel_process->children : nullptr;
      if (kernel_process)
        kernel_process->children = child;
    }
    child = next;
  }
  waiter_tid = process->waiter_thread_id;
  reap_self = process->auto_reap && !waiter_tid &&
              process->parent == kernel_process;
  if (reap_self && kernel_process) {
    ProcessControlBlock **link = &kernel_process->children;
    while (*link && *link != process)
      link = &(*link)->sibling_next;
    if (*link == process)
      *link = process->sibling_next;
  }
  irq_restore(flags);

  while (orphan_zombies) {
    ProcessControlBlock *next = orphan_zombies->sibling_next;
    free_process_arguments(orphan_zombies);
    free(orphan_zombies);
    orphan_zombies = next;
  }
  if (waiter_tid)
    (void)thread_unblock(waiter_tid);
  if (reap_self) {
    free_process_arguments(process);
    free(process);
  }

  flags = irq_save();
  current->state = ThreadState::TERMINATED;
  current->timed_wait = false;
  yield_holding_lock(state);
  local_irq_restore(flags);
  for (;;)
    __asm__ volatile("hlt" ::: "memory");
}

#undef SCHED_LIKELY
#undef SCHED_UNLIKELY

} // namespace Kernel::Scheduler

extern "C" [[noreturn]] void exit(int status) {
  Kernel::Scheduler::process_exit(status);
}
