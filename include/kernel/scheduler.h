#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Kernel::Scheduler {

enum class ThreadState { READY, RUNNING, BLOCKED, TERMINATED };

enum class ThreadPriority { IDLE = 0, NORMAL = 1, HIGH = 2 };
enum class ProcessState : uint32_t { RUNNING = 0, ZOMBIE = 1 };

using ThreadEntry = void (*)(void *);
using ProcessEntry = int (*)(int argc, char **argv, void *context);

struct ProcessControlBlock {
  uint64_t id;
  volatile uint32_t state;
  int exit_status;
  uint32_t active_threads;
  bool exit_requested;
  bool auto_reap;
  uint64_t main_thread_id;
  uint64_t waiter_thread_id;
  // Thread ID owning the controlling terminal's keyboard-input queue.
  // Child processes inherit this ID even though they execute on other threads.
  uint64_t terminal_thread_id;
  ProcessControlBlock *parent;
  ProcessControlBlock *children;
  ProcessControlBlock *sibling_next;
  ProcessEntry entry;
  void *entry_context;
  int argc;
  char **argv;
  void *stdio_context;
  char name[32];
};

struct ThreadControlBlock {
  uint64_t id;
  ProcessControlBlock *process;
  ThreadState state;
  ThreadPriority priority;
  // Soft preference; UINT32_MAX means any CPU. Other CPUs may steal when idle.
  uint32_t preferred_cpu;
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
  int errno_value;
};

struct ApplicationTaskInfo {
  uint64_t id;
  char name[32];
  uint32_t start_ticks;
};

struct KernelIoSchedulingHint {
  ThreadPriority priority;
  uint32_t preferred_cpu;
};

constexpr size_t MAX_APPLICATION_TASKS = 32;
void scheduler_init();
void scheduler_cpu_early_init();
void scheduler_ap_init();
bool prefer_bootstrap_cpu(uint64_t id);
KernelIoSchedulingHint prioritize_current_for_kernel_io();
void restore_current_scheduling(KernelIoSchedulingHint hint);
int thread_create(void (*entry)(),
                  ThreadPriority priority = ThreadPriority::NORMAL);
int thread_create(ThreadEntry entry, void *argument,
                  ThreadPriority priority = ThreadPriority::NORMAL);
// Creates a child process, copying argv and inheriting its parent's terminal.
// Set auto_reap only for detached children that the caller will not wait on.
uint64_t process_spawn(ProcessEntry entry, int argc, const char *const argv[],
                       void *context = nullptr, const char *name = nullptr,
                       ThreadPriority priority = ThreadPriority::NORMAL,
                       uint64_t *main_thread_id = nullptr,
                       bool auto_reap = false);
int process_wait(uint64_t pid, int *exit_status = nullptr);
uint64_t current_process_id();
uint64_t current_parent_process_id();
bool current_process_is_kernel();
void process_set_terminal_thread_id(uint64_t thread_id);
void process_clear_terminal_thread_id(uint64_t thread_id);
uint64_t process_terminal_thread_id();
void *process_stdio_context();
void process_set_stdio_context(void *context);
uint64_t current_thread_id();
int *current_errno_location();
void mark_current_thread_as_application(const char *name = nullptr);
size_t get_application_tasks(ApplicationTaskInfo *tasks, size_t capacity);
bool is_current_thread_application();
void terminate_thread(uint64_t id);
bool should_stop();
void thread_yield();
bool thread_unblock(uint64_t id);
[[noreturn]] void thread_exit();
[[noreturn]] void process_exit(int status = 0);
void thread_block();
void thread_block_on(volatile uint32_t *word, uint32_t expected);
bool thread_block_if(bool (*should_block)());
void thread_sleep_ms(uint32_t milliseconds);
void unblock_io_threads();
bool should_preempt();
uint64_t schedule(uint64_t current_rsp);

} // namespace Kernel::Scheduler
