#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Kernel::Scheduler {

enum class ThreadState { READY, RUNNING, BLOCKED, TERMINATED };

enum class ThreadPriority { IDLE = 0, NORMAL = 1, HIGH = 2 };

struct ThreadControlBlock {
  uint64_t id;
  ThreadState state;
  ThreadPriority priority;
  uint64_t *stack_mem;
  uint64_t rsp;
  // Run-queue and registry links are kept separate.
  ThreadControlBlock *next;
  ThreadControlBlock *registry_prev;
  ThreadControlBlock *registry_next;
  ThreadControlBlock *reap_next;
  ThreadControlBlock *sleep_next;
  volatile bool stop_requested;
  bool application_thread;
  uint32_t wake_tick;
  char application_name[32];
  uint32_t application_start_ticks;
  bool timed_wait;
};

struct ApplicationTaskInfo {
  uint64_t id;
  char name[32];
  uint32_t start_ticks;
};

constexpr size_t MAX_APPLICATION_TASKS = 32;
using ThreadEntry = void (*)(void *);

void scheduler_init();
int thread_create(void (*entry)(),
                  ThreadPriority priority = ThreadPriority::NORMAL);
int thread_create(ThreadEntry entry, void *argument,
                  ThreadPriority priority = ThreadPriority::NORMAL);
uint64_t current_thread_id();
void mark_current_thread_as_application(const char *name = nullptr);
size_t get_application_tasks(ApplicationTaskInfo *tasks, size_t capacity);
bool is_current_thread_application();
void terminate_thread(uint64_t id);
bool should_stop();
void thread_yield();
[[noreturn]] void thread_exit();
void thread_block();
// The short predicate runs with interrupts disabled to avoid lost wakeups.
bool thread_block_if(bool (*should_block)());
void thread_sleep_ms(uint32_t milliseconds);
void unblock_io_threads();
bool should_preempt();
uint64_t schedule(uint64_t current_rsp);

} // namespace Kernel::Scheduler
