#pragma once

#include <arch/x86_64/string.h>
#include <cstddef>
#include <cstdint>
#include <kernel/scheduler.h>

namespace Kernel::Scheduler {

struct ThreadContext {
  uint64_t xstate_ptr;
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
  uint64_t interrupt_number;
  uint64_t error_code;
  uint64_t rip;
  uint64_t cs;
  uint64_t rflags;
  uint64_t rsp;
  uint64_t ss;
} __attribute__((packed));

static_assert(sizeof(ThreadContext) == 184);

// total  = threads queued at this priority.
// pinned = queued threads with preferred_cpu != UINT32_MAX. The pinning APIs
// currently target bootstrap_cpu_id, so pinned is the service-CPU count.
struct RunQueue {
  ThreadControlBlock *head;
  ThreadControlBlock *tail;
  uint32_t total;
  uint32_t pinned;
};

// Exactly one cache line per CPU. The dispatch counters occupy the original
// tail padding, preserving the offsets of all pre-existing members.
struct alignas(64) CpuSchedulerState {
  CpuSchedulerState *self;
  uint32_t cpu_id;
  ThreadControlBlock *current;
  ThreadControlBlock *idle;
  ThreadControlBlock *zombie;
  bool lock_held;
  uint32_t high_dispatches;
  uint32_t idle_dispatches;
};
static_assert(offsetof(CpuSchedulerState, self) == 0);
static_assert(offsetof(CpuSchedulerState, cpu_id) == 8);
static_assert(offsetof(CpuSchedulerState, current) == 16);
static_assert(offsetof(CpuSchedulerState, idle) == 24);
static_assert(offsetof(CpuSchedulerState, zombie) == 32);
static_assert(offsetof(CpuSchedulerState, lock_held) == 40);
static_assert(sizeof(CpuSchedulerState) == 64);

static constexpr unsigned PRIORITY_COUNT = 3;
static constexpr size_t THREAD_STACK_SIZE = 64 * 1024;
static constexpr size_t IDLE_STACK_SIZE = 16 * 1024;
static constexpr size_t XSTATE_SIZE = 832;
static constexpr uint32_t MAX_SLEEP_MS = 0x7FFFFFFFU;
static constexpr unsigned HIGH_PRIORITY_DISPATCH_LIMIT = 7;
static constexpr unsigned IDLE_PRIORITY_DISPATCH_INTERVAL = 63;
static constexpr uint64_t RFLAGS_IF = 1ull << 9;
static constexpr size_t MAX_APIC_IDS = 256;

extern RunQueue run_queues[PRIORITY_COUNT];
extern uint32_t ready_priorities;
extern CpuSchedulerState cpu_states[MAX_APIC_IDS];
extern ThreadControlBlock idle_thread_storage[MAX_APIC_IDS];
extern ThreadControlBlock *thread_list_head;
extern ThreadControlBlock *reap_pending;
extern uint64_t next_pid;
extern bool scheduler_initialized;

extern "C" void thread_start_wrapper(void (*)());
extern "C" void thread_start_arg_wrapper(ThreadEntry, void *);

uint64_t local_irq_save();
void local_irq_restore(uint64_t flags);
uint64_t irq_save();
void irq_restore(uint64_t flags);
void yield_holding_lock(CpuSchedulerState &state);
CpuSchedulerState &cpu_state();
unsigned priority_index(ThreadPriority priority);
void enqueue(ThreadControlBlock *thread);
void remove_sleeper(ThreadControlBlock *thread);
void remove_blocked(ThreadControlBlock *thread);
void reap_deferred();

inline void register_thread(ThreadControlBlock *thread) {
  thread->registry_prev = nullptr;
  thread->registry_next = thread_list_head;
  if (thread_list_head)
    thread_list_head->registry_prev = thread;
  thread_list_head = thread;
}

inline bool initialize_context(ThreadControlBlock *thread, uint64_t *stack_mem,
                               size_t stack_size, void (*entry)(),
                               ThreadEntry arg_entry, void *argument) {
  if (!thread || !stack_mem ||
      stack_size < sizeof(ThreadContext) + XSTATE_SIZE + 64)
    return false;

  const uint64_t stack_begin = reinterpret_cast<uint64_t>(stack_mem);
  uint64_t stack_top = (stack_begin + stack_size) & ~uint64_t{0xF};
  if ((stack_top & 0xF) == 0)
    stack_top -= sizeof(uint64_t);

  auto *context =
      reinterpret_cast<ThreadContext *>(stack_top - sizeof(ThreadContext));
  const uint64_t xstate_addr =
      (reinterpret_cast<uint64_t>(context) - XSTATE_SIZE) & ~uint64_t{0x3F};
  if (xstate_addr < stack_begin)
    return false;

  memset(context, 0, sizeof(ThreadContext));
  memset(reinterpret_cast<void *>(xstate_addr), 0, XSTATE_SIZE);
  *reinterpret_cast<uint16_t *>(xstate_addr) = 0x037F;
  *reinterpret_cast<uint32_t *>(xstate_addr + 24) = 0x1F80;

  context->xstate_ptr = xstate_addr;
  if (arg_entry) {
    context->rdi = reinterpret_cast<uint64_t>(arg_entry);
    context->rsi = reinterpret_cast<uint64_t>(argument);
    context->rip = reinterpret_cast<uint64_t>(thread_start_arg_wrapper);
  } else {
    context->rdi = reinterpret_cast<uint64_t>(entry);
    context->rip = reinterpret_cast<uint64_t>(thread_start_wrapper);
  }

  context->cs = 0x08;
  context->rflags = 0x202;
  context->rsp = stack_top;
  context->ss = 0x10;
  thread->stack_mem = stack_mem;
  thread->rsp = reinterpret_cast<uint64_t>(context);
  thread->reap_next = nullptr;
  thread->preferred_cpu = UINT32_MAX;
  return true;
}

} // namespace Kernel::Scheduler
