#include <algorithm>
#include <atomic>
#include <kernel/debug/debug.h>
#include <kernel/drivers/tsc_deadline.h>
#include <kernel/memory/heap.h>
#include <kernel/mtask/internal.h>
#include <stdlib.h>

namespace Kernel::Scheduler {

#define SCHED_LIKELY(x) __builtin_expect(!!(x), 1)
#define SCHED_UNLIKELY(x) __builtin_expect(!!(x), 0)

RunQueue run_queues[PRIORITY_COUNT] = {};
uint32_t ready_priorities = 0;

CpuSchedulerState cpu_states[MAX_APIC_IDS]{};
ThreadControlBlock idle_thread_storage[MAX_APIC_IDS]{};
alignas(64) static uint8_t idle_stack_mem[MAX_APIC_IDS][IDLE_STACK_SIZE];

// Isolate the contended word from scheduler metadata.
struct alignas(64) SchedLock {
  std::atomic<uint32_t> locked{0};
  uint8_t pad[60]{};
};
static SchedLock scheduler_lock_storage{};

ThreadControlBlock *thread_list_head = nullptr;
ThreadControlBlock *reap_pending = nullptr;
static ThreadControlBlock *sleep_queue = nullptr;
static ThreadControlBlock *blocked_list_head = nullptr;
uint64_t next_pid = 1;
bool scheduler_initialized = false;
static uint32_t bootstrap_cpu_id = 0;

[[noreturn]] static void idle_loop() {
  for (;;)
    __asm__ volatile("sti; hlt" ::: "memory");
}

// Used only while installing per-CPU state, never on a scheduling entry.
static inline size_t cpu_index() {
  uint32_t eax = 1, ebx, ecx, edx;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
  return (ebx >> 24) & 0xffu;
}

static inline void install_cpu_state(size_t index) {
  if (SCHED_UNLIKELY(index >= MAX_APIC_IDS))
    return;
  CpuSchedulerState &state = cpu_states[index];
  state.self = &state;
  state.cpu_id = static_cast<uint32_t>(index);
  const uint64_t base = reinterpret_cast<uint64_t>(&state);
  __asm__ volatile("wrmsr"
                   :
                   : "c"(0xC0000101u), "a"(static_cast<uint32_t>(base)),
                     "d"(static_cast<uint32_t>(base >> 32))
                   : "memory");
}

CpuSchedulerState &cpu_state() {
  CpuSchedulerState *state;
  __asm__ volatile("movq %%gs:0, %0" : "=r"(state));
  return *state;
}

// --- locking ---------------------------------------------------------------

static inline void lock_acquire() {
  auto &word = scheduler_lock_storage.locked;
  for (;;) {
    uint32_t expected = 0;
    if (SCHED_LIKELY(word.compare_exchange_weak(
            expected, 1, std::memory_order_acquire, std::memory_order_relaxed)))
      return;
    do {
      __builtin_ia32_pause();
    } while (word.load(std::memory_order_relaxed));
  }
}

static inline void lock_release() {
  scheduler_lock_storage.locked.store(0, std::memory_order_release);
}

uint64_t local_irq_save() {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
  return flags;
}

void local_irq_restore(uint64_t flags) {
  if (flags & RFLAGS_IF)
    __asm__ volatile("sti" : : : "memory");
}

uint64_t irq_save() {
  const uint64_t flags = local_irq_save();
  lock_acquire();
  return flags;
}

void irq_restore(uint64_t flags) {
  lock_release();
  local_irq_restore(flags);
}

static inline void irq_restore_flags(uint64_t flags) {
  local_irq_restore(flags);
}

// Blocking paths used to: enqueue self on a wait list, DROP the lock, then
// `int 0x30`. In that window another CPU could wake the thread and dispatch it
// from its stale saved RSP while this CPU was still running on its stack.
// Now the lock stays held across the interrupt and schedule() adopts it, which
// closes that race and also saves one lock release+reacquire per handoff.
void yield_holding_lock(CpuSchedulerState &state) {
  state.lock_held = true;
  __asm__ volatile("int $0x30" ::: "memory");
}

static inline bool is_idle_thread(const ThreadControlBlock *thread) {
  const uintptr_t p = reinterpret_cast<uintptr_t>(thread);
  const uintptr_t first = reinterpret_cast<uintptr_t>(idle_thread_storage);
  return p >= first && p < first + sizeof(idle_thread_storage);
}

unsigned priority_index(ThreadPriority priority) {
  const unsigned index = static_cast<unsigned>(priority);
  return index < PRIORITY_COUNT ? index
                                : static_cast<unsigned>(ThreadPriority::NORMAL);
}

static inline bool is_pinned(const ThreadControlBlock *thread) {
  return thread->preferred_cpu != UINT32_MAX;
}

void enqueue(ThreadControlBlock *thread) {
  if (!thread || is_idle_thread(thread) ||
      thread->state == ThreadState::TERMINATED)
    return;

  const unsigned index = priority_index(thread->priority);
  RunQueue &queue = run_queues[index];
  thread->next = nullptr;
  thread->state = ThreadState::READY;

  if (queue.tail)
    queue.tail->next = thread;
  else
    queue.head = thread;

  queue.tail = thread;
  ++queue.total;
  if (is_pinned(thread))
    ++queue.pinned;
  ready_priorities |= 1u << index;
}

static void insert_sleeper(ThreadControlBlock *thread) {
  ThreadControlBlock **link = &sleep_queue;
  while (*link &&
         static_cast<int32_t>((*link)->wake_tick - thread->wake_tick) <= 0)
    link = &(*link)->sleep_next;

  thread->sleep_next = *link;
  *link = thread;
}

void remove_sleeper(ThreadControlBlock *thread) {
  ThreadControlBlock **link = &sleep_queue;
  while (*link && *link != thread)
    link = &(*link)->sleep_next;

  if (*link == thread) {
    *link = thread->sleep_next;
    thread->sleep_next = nullptr;
  }
}

static void wake_sleepers(uint32_t now) {
  while (sleep_queue &&
         static_cast<int32_t>(now - sleep_queue->wake_tick) >= 0) {
    ThreadControlBlock *thread = sleep_queue;
    sleep_queue = thread->sleep_next;
    thread->sleep_next = nullptr;
    thread->timed_wait = false;
    enqueue(thread);
  }
}

static void insert_blocked(ThreadControlBlock *thread) {
  thread->sleep_next = blocked_list_head;
  blocked_list_head = thread;
}

void remove_blocked(ThreadControlBlock *thread) {
  ThreadControlBlock **link = &blocked_list_head;
  while (*link && *link != thread)
    link = &(*link)->sleep_next;

  if (*link == thread) {
    *link = thread->sleep_next;
    thread->sleep_next = nullptr;
  }
}

// O(1) replacement for the old queue_has_match() list walks.
static inline bool queue_available(unsigned index, bool skip_preferred) {
  const RunQueue &queue = run_queues[index];
  return skip_preferred ? queue.total > queue.pinned : queue.total != 0;
}

static ThreadControlBlock *dequeue_priority(unsigned index, uint32_t cpu_id,
                                            bool preferred_only = false,
                                            bool skip_preferred = false) {
  if (SCHED_UNLIKELY(index >= PRIORITY_COUNT))
    return nullptr;
  const uint32_t bit = 1u << index;
  if (SCHED_UNLIKELY(!(ready_priorities & bit)))
    return nullptr;

  RunQueue &queue = run_queues[index];
  if (!preferred_only && skip_preferred && !queue.pinned)
    skip_preferred = false;

  // The dominant path is an O(1) head pop, with no previous-node tracking.
  if (!preferred_only && !skip_preferred) {
    ThreadControlBlock *const thread = queue.head;
    if (SCHED_UNLIKELY(!thread)) {
      queue.tail = nullptr;
      ready_priorities &= ~bit;
      return nullptr;
    }

    queue.head = thread->next;
    if (SCHED_UNLIKELY(!queue.head)) {
      queue.tail = nullptr;
      ready_priorities &= ~bit;
    }
    --queue.total;
    if (thread->preferred_cpu != UINT32_MAX)
      --queue.pinned;
    thread->next = nullptr;
    return thread;
  }

  ThreadControlBlock *previous = nullptr;
  ThreadControlBlock *thread = queue.head;
  while (thread) {
    const uint32_t preferred = thread->preferred_cpu;
    if ((!preferred_only || preferred == cpu_id) &&
        (!skip_preferred || preferred == UINT32_MAX || preferred == cpu_id))
      break;
    previous = thread;
    thread = thread->next;
  }

  if (SCHED_UNLIKELY(!thread)) {
    if (!queue.head) {
      queue.tail = nullptr;
      ready_priorities &= ~bit;
    }
    return nullptr;
  }

  ThreadControlBlock *const next = thread->next;
  if (previous)
    previous->next = next;
  else
    queue.head = next;
  if (queue.tail == thread)
    queue.tail = previous;
  if (SCHED_UNLIKELY(!queue.head))
    ready_priorities &= ~bit;

  --queue.total;
  if (thread->preferred_cpu != UINT32_MAX)
    --queue.pinned;
  thread->next = nullptr;
  return thread;
}

static ThreadControlBlock *dequeue_fair(CpuSchedulerState &state) {
  constexpr unsigned HIGH = static_cast<unsigned>(ThreadPriority::HIGH);
  constexpr unsigned NORMAL = static_cast<unsigned>(ThreadPriority::NORMAL);
  constexpr unsigned IDLE = static_cast<unsigned>(ThreadPriority::IDLE);

  const uint32_t cpu_id = state.cpu_id;
  if (SCHED_UNLIKELY(cpu_id >= MAX_APIC_IDS))
    return nullptr;

  uint32_t &high_count = state.high_dispatches;
  uint32_t &idle_count = state.idle_dispatches;
  const bool service_cpu = cpu_id == bootstrap_cpu_id;
  bool skip_preferred = !service_cpu;
  bool high_ready, normal_ready, idle_ready;

  if (service_cpu) {
    const uint32_t ready = ready_priorities;
    high_ready = (ready & (1u << HIGH)) != 0;
    normal_ready = (ready & (1u << NORMAL)) != 0;
    idle_ready = (ready & (1u << IDLE)) != 0;
  } else {
    high_ready = queue_available(HIGH, true);
    normal_ready = queue_available(NORMAL, true);
    idle_ready = queue_available(IDLE, true);
  }

  if (service_cpu && run_queues[HIGH].pinned &&
      (!normal_ready || high_count < HIGH_PRIORITY_DISPATCH_LIMIT)) {
    if (ThreadControlBlock *service = dequeue_priority(HIGH, cpu_id, true)) {
      if (high_count < HIGH_PRIORITY_DISPATCH_LIMIT)
        ++high_count;
      if (idle_ready)
        ++idle_count;
      else
        idle_count = 0;
      return service;
    }
  }

  if (!high_ready && !normal_ready && !idle_ready) {
    skip_preferred = false;
    high_ready = queue_available(HIGH, false);
    normal_ready = queue_available(NORMAL, false);
    idle_ready = queue_available(IDLE, false);
  }

  if (high_ready && !normal_ready && !idle_ready) {
    high_count = idle_count = 0;
    return dequeue_priority(HIGH, cpu_id, false, skip_preferred);
  }
  if (normal_ready && !high_ready && !idle_ready) {
    high_count = idle_count = 0;
    return dequeue_priority(NORMAL, cpu_id, false, skip_preferred);
  }
  if (idle_ready && !high_ready && !normal_ready) {
    high_count = idle_count = 0;
    return dequeue_priority(IDLE, cpu_id, false, skip_preferred);
  }

  if (idle_ready && (high_ready || normal_ready) &&
      idle_count >= IDLE_PRIORITY_DISPATCH_INTERVAL) {
    high_count = idle_count = 0;
    return dequeue_priority(IDLE, cpu_id, false, skip_preferred);
  }

  if (high_ready && normal_ready &&
      high_count >= HIGH_PRIORITY_DISPATCH_LIMIT) {
    high_count = 0;
    if (idle_ready)
      ++idle_count;
    else
      idle_count = 0;
    return dequeue_priority(NORMAL, cpu_id, false, skip_preferred);
  }

  if (high_ready) {
    if (high_count < HIGH_PRIORITY_DISPATCH_LIMIT)
      ++high_count;
    if (idle_ready)
      ++idle_count;
    else
      idle_count = 0;
    return dequeue_priority(HIGH, cpu_id, false, skip_preferred);
  }

  high_count = 0;
  if (normal_ready) {
    if (idle_ready)
      ++idle_count;
    else
      idle_count = 0;
    return dequeue_priority(NORMAL, cpu_id, false, skip_preferred);
  }

  idle_count = 0;
  return idle_ready ? dequeue_priority(IDLE, cpu_id, false, skip_preferred)
                    : nullptr;
}

void scheduler_init() {
  if (scheduler_initialized) {
    Kernel::Debug::log("scheduler init skipped: already initialized");
    return;
  }

  // Heap allocation happens before disabling interrupts; the critical section
  // below only publishes fully initialized scheduler state.
  auto *main_tcb =
      static_cast<ThreadControlBlock *>(malloc(sizeof(ThreadControlBlock)));
  if (!main_tcb) {
    Kernel::Debug::log("ERROR: scheduler init failed: main task allocation");
    return;
  }

  memset(main_tcb, 0, sizeof(ThreadControlBlock));
  const size_t index = cpu_index();
  if (SCHED_UNLIKELY(index >= MAX_APIC_IDS)) {
    free(main_tcb);
    Kernel::Debug::log("ERROR: scheduler init failed: APIC ID out of range");
    return;
  }
  memset(&idle_thread_storage[index], 0, sizeof(ThreadControlBlock));
  if (!initialize_context(&idle_thread_storage[index],
                          reinterpret_cast<uint64_t *>(idle_stack_mem[index]),
                          IDLE_STACK_SIZE, idle_loop, nullptr, nullptr)) {
    free(main_tcb);
    Kernel::Debug::log("ERROR: scheduler init failed: idle context setup");
    return;
  }
  idle_thread_storage[index].id = 0;
  idle_thread_storage[index].state = ThreadState::READY;
  idle_thread_storage[index].priority = ThreadPriority::IDLE;

  const uint64_t flags = irq_save();
  bootstrap_cpu_id = static_cast<uint32_t>(index);
  main_tcb->id = next_pid++;
  main_tcb->state = ThreadState::RUNNING;
  main_tcb->priority = ThreadPriority::IDLE;
  main_tcb->preferred_cpu = UINT32_MAX;
  main_tcb->stack_mem = nullptr;
  main_tcb->rsp = 0;
  main_tcb->next = nullptr;
  main_tcb->reap_next = nullptr;
  main_tcb->stop_requested = false;
  main_tcb->wake_tick = 0;
  main_tcb->timed_wait = false;
  main_tcb->sleep_next = nullptr;

  register_thread(main_tcb);
  cpu_states[index].idle = &idle_thread_storage[index];
  cpu_states[index].current = main_tcb;
  install_cpu_state(index);
  scheduler_initialized = true;
  irq_restore(flags);
  Kernel::Debug::log("scheduler initialized; bootstrap task registered");
  Kernel::Debug::log_dec("bootstrap_task_id", main_tcb->id);
}

void scheduler_cpu_early_init() {
  const size_t index = cpu_index();
  if (index < MAX_APIC_IDS)
    install_cpu_state(index);
}

bool prefer_bootstrap_cpu(uint64_t id) {
  if (!id)
    return false;
  const uint64_t flags = irq_save();
  for (ThreadControlBlock *thread = thread_list_head; thread;
       thread = thread->registry_next) {
    if (thread->id == id && thread->state != ThreadState::TERMINATED) {
      // A queued (READY) thread is already counted in its run queue; keep the
      // pinned counter in sync when its affinity changes underneath it.
      if (thread->state == ThreadState::READY && !is_pinned(thread))
        ++run_queues[priority_index(thread->priority)].pinned;
      thread->preferred_cpu = bootstrap_cpu_id;
      irq_restore(flags);
      return true;
    }
  }
  irq_restore(flags);
  return false;
}

KernelIoSchedulingHint prioritize_current_for_kernel_io() {
  const uint64_t flags = irq_save();
  CpuSchedulerState &state = cpu_state();
  ThreadControlBlock *current = state.current;
  if (!current) {
    irq_restore(flags);
    return {ThreadPriority::NORMAL, UINT32_MAX};
  }
  const KernelIoSchedulingHint previous{current->priority,
                                        current->preferred_cpu};
  if (current != state.idle) {
    current->priority = ThreadPriority::HIGH;
    current->preferred_cpu = bootstrap_cpu_id;
  }
  irq_restore(flags);
  return previous;
}

void restore_current_scheduling(KernelIoSchedulingHint hint) {
  const uint64_t flags = irq_save();
  if (ThreadControlBlock *current = cpu_state().current) {
    current->priority = hint.priority;
    current->preferred_cpu = hint.preferred_cpu;
  }
  irq_restore(flags);
}

void scheduler_ap_init() {
  const size_t index = cpu_index();
  if (SCHED_UNLIKELY(index >= MAX_APIC_IDS))
    return;

  CpuSchedulerState &state = cpu_states[index];
  if (!state.current) {
    ThreadControlBlock &idle = idle_thread_storage[index];
    memset(&idle, 0, sizeof(idle));
    if (!initialize_context(&idle,
                            reinterpret_cast<uint64_t *>(idle_stack_mem[index]),
                            IDLE_STACK_SIZE, idle_loop, nullptr, nullptr))
      return;
    idle.id = 0;
    idle.state = ThreadState::READY;
    idle.priority = ThreadPriority::IDLE;

    const uint64_t flags = irq_save();
    if (!state.current) {
      state.idle = &idle;
      state.current = &idle;
    }
    install_cpu_state(index);
    irq_restore(flags);
    return;
  }

  install_cpu_state(index);
}

void thread_yield() {
  reap_deferred();
  __asm__ volatile("int $0x30" ::: "memory");
}

void thread_block() {
  reap_deferred();
  const uint64_t flags = irq_save();
  CpuSchedulerState &state = cpu_state();
  ThreadControlBlock *current_thread = state.current;
  if (!current_thread || current_thread == state.idle) {
    irq_restore(flags);
    return;
  }

  current_thread->state = ThreadState::BLOCKED;
  current_thread->timed_wait = false;
  insert_blocked(current_thread);
  yield_holding_lock(state);
  irq_restore_flags(flags);
}

auto thread_block_if(bool (*should_block)()) -> bool {
  reap_deferred();
  if (!should_block || !should_block())
    return false;

  const uint64_t flags = irq_save();
  CpuSchedulerState &state = cpu_state();
  ThreadControlBlock *current_thread = state.current;
  if (!current_thread || current_thread == state.idle || !should_block()) {
    irq_restore(flags);
    return false;
  }

  current_thread->state = ThreadState::BLOCKED;
  current_thread->timed_wait = false;
  insert_blocked(current_thread);
  yield_holding_lock(state);
  irq_restore_flags(flags);
  return true;
}

void thread_sleep_ms(uint32_t milliseconds) {
  reap_deferred();
  if (milliseconds == 0)
    milliseconds = 1;
  milliseconds = std::min(milliseconds, MAX_SLEEP_MS);

  const uint64_t flags = irq_save();
  CpuSchedulerState &state = cpu_state();
  ThreadControlBlock *current_thread = state.current;
  if (!current_thread || current_thread == state.idle) {
    irq_restore(flags);
    return;
  }

  const uint32_t now =
      static_cast<uint32_t>(Kernel::Drivers::TSCDeadline::get_ticks());
  current_thread->wake_tick = now + milliseconds;
  current_thread->timed_wait = true;
  insert_sleeper(current_thread);
  current_thread->state = ThreadState::BLOCKED;

  yield_holding_lock(state);
  irq_restore_flags(flags);
}

void unblock_io_threads() {
  const uint64_t flags = irq_save();
  ThreadControlBlock *list = blocked_list_head;
  blocked_list_head = nullptr;

  while (list) {
    ThreadControlBlock *thread = list;
    list = thread->sleep_next;
    thread->sleep_next = nullptr;
    enqueue(thread);
  }
  irq_restore(flags);
}

bool thread_unblock(uint64_t id) {
  if (id == 0)
    return false;

  const uint64_t flags = irq_save();
  // Single pass: unlink while searching (old code searched, then searched
  // again inside remove_blocked()).
  for (ThreadControlBlock **link = &blocked_list_head; *link;
       link = &(*link)->sleep_next) {
    ThreadControlBlock *thread = *link;
    if (thread->id == id) {
      *link = thread->sleep_next;
      thread->sleep_next = nullptr;
      enqueue(thread);
      irq_restore(flags);
      return true;
    }
  }
  irq_restore(flags);
  return false;
}

auto should_preempt() -> bool { return true; }

auto schedule(uint64_t current_rsp) -> uint64_t {
  CpuSchedulerState &state = cpu_state();
  uint64_t flags = 0;
  const bool inherited_lock = state.lock_held;
  if (inherited_lock)
    state.lock_held = false;
  else
    flags = irq_save();

  ThreadControlBlock *const previous = state.current;
  if (SCHED_UNLIKELY(!previous)) {
    irq_restore(flags);
    return current_rsp;
  }

  const bool previous_is_idle = previous == state.idle;
  uint32_t now = 0;
  bool have_now = false;

  // No candidate can run: retain the current stack without touching its TCB,
  // the sleeper list, or the reaper list. Only inspect time when a sleeper
  // exists; a future deadline means this interrupt cannot cause a switch.
  if (!inherited_lock && !state.zombie && !ready_priorities &&
      (previous_is_idle || previous->state == ThreadState::RUNNING)) {
    if (!sleep_queue) {
      if (previous_is_idle)
        previous->state = ThreadState::RUNNING;
      irq_restore(flags);
      return current_rsp;
    }
    now = static_cast<uint32_t>(Kernel::Drivers::TSCDeadline::get_ticks());
    have_now = true;
    if (static_cast<int32_t>(now - sleep_queue->wake_tick) < 0) {
      if (previous_is_idle)
        previous->state = ThreadState::RUNNING;
      irq_restore(flags);
      return current_rsp;
    }
  }

  // Publish the saved stack before making this thread visible in a run queue.
  previous->rsp = current_rsp;

  if (state.zombie) {
    state.zombie->reap_next = reap_pending;
    reap_pending = state.zombie;
    state.zombie = nullptr;
  }

  if (sleep_queue) {
    if (!have_now)
      now = static_cast<uint32_t>(Kernel::Drivers::TSCDeadline::get_ticks());
    if (SCHED_UNLIKELY(static_cast<int32_t>(now - sleep_queue->wake_tick) >= 0))
      wake_sleepers(now);
  }

  if (!previous_is_idle) {
    if (previous->state == ThreadState::RUNNING) {
      if (!ready_priorities) {
        irq_restore(flags);
        return current_rsp;
      }
      enqueue(previous);
    } else if (previous->state == ThreadState::TERMINATED) {
      state.zombie = previous;
    }
  } else if (!ready_priorities) {
    previous->state = ThreadState::RUNNING;
    irq_restore(flags);
    return current_rsp;
  }

  ThreadControlBlock *next = dequeue_fair(state);
  if (!next)
    next = state.idle;

  if (SCHED_UNLIKELY(!next)) {
    irq_restore(flags);
    return current_rsp;
  }

  next->state = ThreadState::RUNNING;
  state.current = next;
  const uint64_t next_rsp = next->rsp;
  irq_restore(flags);
  return next_rsp;
}

} // namespace Kernel::Scheduler
