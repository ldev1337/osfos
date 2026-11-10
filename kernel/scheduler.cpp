#include <algorithm>
#include <arch/x86_64/string.h>
#include <cstddef>
#include <cstdint>
#include <kernel/drivers/pit.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>
#include <atomic>

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

struct RunQueue {
  ThreadControlBlock *head;
  ThreadControlBlock *tail;
};

static constexpr unsigned PRIORITY_COUNT = 3;
static constexpr size_t THREAD_STACK_SIZE = 64 * 1024;
static constexpr size_t IDLE_STACK_SIZE = 16 * 1024;
static constexpr size_t XSTATE_SIZE = 832;
static constexpr uint32_t MAX_SLEEP_MS = 0x7FFFFFFFU;
static constexpr unsigned HIGH_PRIORITY_DISPATCH_LIMIT = 7;
static constexpr unsigned IDLE_PRIORITY_DISPATCH_INTERVAL = 63;

static RunQueue run_queues[PRIORITY_COUNT] = {};
static uint32_t ready_priorities = 0;
static unsigned consecutive_high_dispatches = 0;
static unsigned dispatches_since_idle = 0;
struct CpuSchedulerState {
  ThreadControlBlock *current;
  ThreadControlBlock *idle;
};
static constexpr size_t MAX_APIC_IDS = 256;
static CpuSchedulerState cpu_states[MAX_APIC_IDS]{};
static ThreadControlBlock idle_thread_storage[MAX_APIC_IDS]{};
alignas(64) static uint8_t idle_stack_mem[MAX_APIC_IDS][IDLE_STACK_SIZE];
static std::atomic_flag scheduler_lock = ATOMIC_FLAG_INIT;
static ThreadControlBlock *thread_list_head = nullptr;
static ThreadControlBlock *reap_pending = nullptr;
static ThreadControlBlock *sleep_queue = nullptr;
static ThreadControlBlock *blocked_list_head = nullptr;
static uint64_t next_pid = 1;
static bool scheduler_initialized = false;
static uint32_t bootstrap_cpu_id = 0;

[[noreturn]] static void idle_loop() {
  for (;;)
    __asm__ volatile("sti; hlt" ::: "memory");
}

static inline size_t cpu_index() {
  uint32_t eax = 1, ebx, ecx, edx;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
  return (ebx >> 24) & 0xffu;
}

static inline CpuSchedulerState &cpu_state() {
  return cpu_states[cpu_index()];
}

extern "C" void thread_start_wrapper(void (*)());
extern "C" void thread_start_arg_wrapper(ThreadEntry, void *);

static inline uint64_t irq_save() {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
  while (scheduler_lock.test_and_set(std::memory_order_acquire))
    __asm__ volatile("pause");
  return flags;
}

static inline void irq_restore(uint64_t flags) {
  scheduler_lock.clear(std::memory_order_release);
  if (flags & (1ull << 9))
    __asm__ volatile("sti" : : : "memory");
}

static inline void irq_restore_flags(uint64_t flags) {
  if (flags & (1ull << 9))
    __asm__ volatile("sti" : : : "memory");
}

// Used while the caller already has interrupts disabled. The saved interrupt
// frame therefore resumes with IF=0, and irq_restore() runs after the thread
// has actually been scheduled again.
static inline void yield_with_irqs_disabled() {
  __asm__ volatile("int $0x30" ::: "memory");
}

static inline unsigned priority_index(ThreadPriority priority) {
  const unsigned index = static_cast<unsigned>(priority);
  return index < PRIORITY_COUNT ? index
                                : static_cast<unsigned>(ThreadPriority::NORMAL);
}

static void enqueue(ThreadControlBlock *thread) {
  if (!thread || thread == cpu_state().idle ||
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

static void remove_sleeper(ThreadControlBlock *thread) {
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

static void remove_blocked(ThreadControlBlock *thread) {
  ThreadControlBlock **link = &blocked_list_head;
  while (*link && *link != thread)
    link = &(*link)->sleep_next;

  if (*link == thread) {
    *link = thread->sleep_next;
    thread->sleep_next = nullptr;
  }
}

static bool queue_has_match(unsigned index, uint32_t cpu_id,
                            bool preferred_only, bool skip_preferred) {
  if (index >= PRIORITY_COUNT)
    return false;
  for (ThreadControlBlock *thread = run_queues[index].head; thread;
       thread = thread->next) {
    const bool is_preferred = thread->preferred_cpu != UINT32_MAX;
    if (preferred_only && thread->preferred_cpu != cpu_id)
      continue;
    if (skip_preferred && is_preferred && thread->preferred_cpu != cpu_id)
      continue;
    return true;
  }
  return false;
}

static ThreadControlBlock *dequeue_priority(unsigned index, uint32_t cpu_id,
                                            bool preferred_only = false,
                                            bool skip_preferred = false) {
  if (index >= PRIORITY_COUNT || !(ready_priorities & (1u << index)))
    return nullptr;

  RunQueue &queue = run_queues[index];
  ThreadControlBlock *previous = nullptr;
  ThreadControlBlock *thread = queue.head;
  while (thread) {
    const bool is_preferred = thread->preferred_cpu != UINT32_MAX;
    if ((!preferred_only || thread->preferred_cpu == cpu_id) &&
        (!skip_preferred || !is_preferred || thread->preferred_cpu == cpu_id))
      break;
    previous = thread;
    thread = thread->next;
  }
  if (!thread) {
    if (!queue.head) {
      queue.tail = nullptr;
      ready_priorities &= ~(1u << index);
    }
    return nullptr;
  }

  if (previous)
    previous->next = thread->next;
  else
    queue.head = thread->next;
  if (queue.tail == thread)
    queue.tail = previous;
  if (!queue.head)
    ready_priorities &= ~(1u << index);
  thread->next = nullptr;
  return thread;
}

static ThreadControlBlock *dequeue_fair() {
  constexpr unsigned HIGH = static_cast<unsigned>(ThreadPriority::HIGH);
  constexpr unsigned NORMAL = static_cast<unsigned>(ThreadPriority::NORMAL);
  constexpr unsigned IDLE = static_cast<unsigned>(ThreadPriority::IDLE);

  const uint32_t cpu_id = static_cast<uint32_t>(cpu_index());
  const bool is_service_cpu = cpu_id == bootstrap_cpu_id;
  bool skip_preferred = !is_service_cpu;
  bool high_ready = queue_has_match(HIGH, cpu_id, false, skip_preferred);
  bool normal_ready = queue_has_match(NORMAL, cpu_id, false, skip_preferred);
  bool idle_ready = queue_has_match(IDLE, cpu_id, false, skip_preferred);

  // Kernel service work gets first access to the BSP's dispatch budget. The
  // normal high-priority fairness limit still forces progress for other work.
  if (is_service_cpu &&
      queue_has_match(HIGH, cpu_id, true, false) &&
      (!normal_ready || consecutive_high_dispatches <
                            HIGH_PRIORITY_DISPATCH_LIMIT)) {
    ThreadControlBlock *service = dequeue_priority(HIGH, cpu_id, true, false);
    if (service) {
      if (consecutive_high_dispatches < HIGH_PRIORITY_DISPATCH_LIMIT)
        ++consecutive_high_dispatches;
      if (idle_ready)
        ++dispatches_since_idle;
      else
        dispatches_since_idle = 0;
      return service;
    }
  }

  // Do not let busy workers on another CPU consume the preferred core's
  // services while ordinary work is available. If only service work remains,
  // any idle core may steal it.
  if (!high_ready && !normal_ready && !idle_ready) {
    skip_preferred = false;
    high_ready = queue_has_match(HIGH, cpu_id, false, false);
    normal_ready = queue_has_match(NORMAL, cpu_id, false, false);
    idle_ready = queue_has_match(IDLE, cpu_id, false, false);
  }

  // FAST PATH: If exactly one priority level is active, skip all fairness
  // checks and branch overhead.
  if (high_ready && !normal_ready && !idle_ready) {
    consecutive_high_dispatches = 0;
    dispatches_since_idle = 0;
    return dequeue_priority(HIGH, cpu_id, false, skip_preferred);
  }
  if (normal_ready && !high_ready && !idle_ready) {
    consecutive_high_dispatches = 0;
    dispatches_since_idle = 0;
    return dequeue_priority(NORMAL, cpu_id, false, skip_preferred);
  }
  if (idle_ready && !high_ready && !normal_ready) {
    consecutive_high_dispatches = 0;
    dispatches_since_idle = 0;
    return dequeue_priority(IDLE, cpu_id, false, skip_preferred);
  }

  // Background-priority work gets one dispatch after a bounded interval.
  if (idle_ready && (high_ready || normal_ready) &&
      dispatches_since_idle >= IDLE_PRIORITY_DISPATCH_INTERVAL) {
    dispatches_since_idle = 0;
    consecutive_high_dispatches = 0;
    return dequeue_priority(IDLE, cpu_id, false, skip_preferred);
  }

  // A continuously busy HIGH queue cannot starve NORMAL work.
  if (high_ready && normal_ready &&
      consecutive_high_dispatches >= HIGH_PRIORITY_DISPATCH_LIMIT) {
    consecutive_high_dispatches = 0;
    if (idle_ready)
      ++dispatches_since_idle;
    else
      dispatches_since_idle = 0;
    return dequeue_priority(NORMAL, cpu_id, false, skip_preferred);
  }

  if (high_ready) {
    if (consecutive_high_dispatches < HIGH_PRIORITY_DISPATCH_LIMIT)
      ++consecutive_high_dispatches;
    if (idle_ready)
      ++dispatches_since_idle;
    else
      dispatches_since_idle = 0;
    return dequeue_priority(HIGH, cpu_id, false, skip_preferred);
  }

  consecutive_high_dispatches = 0;
  if (normal_ready) {
    if (idle_ready)
      ++dispatches_since_idle;
    else
      dispatches_since_idle = 0;
    return dequeue_priority(NORMAL, cpu_id, false, skip_preferred);
  }

  dispatches_since_idle = 0;
  return idle_ready ? dequeue_priority(IDLE, cpu_id, false, skip_preferred)
                    : nullptr;
}

static void register_thread(ThreadControlBlock *thread) {
  thread->registry_prev = nullptr;
  thread->registry_next = thread_list_head;
  if (thread_list_head)
    thread_list_head->registry_prev = thread;
  thread_list_head = thread;
}

static void unlink_thread(ThreadControlBlock *thread) {
  if (thread->registry_prev)
    thread->registry_prev->registry_next = thread->registry_next;
  else
    thread_list_head = thread->registry_next;

  if (thread->registry_next)
    thread->registry_next->registry_prev = thread->registry_prev;
}

static void reap_deferred() {
  const uint64_t flags = irq_save();
  ThreadControlBlock *pending = reap_pending;
  reap_pending = nullptr;

  for (ThreadControlBlock *thread = pending; thread; thread = thread->reap_next)
    unlink_thread(thread);

  irq_restore(flags);

  // Never free a thread's stack from its own execution context. The scheduler
  // puts exited threads here; a later creator reclaims them safely.
  while (pending) {
    ThreadControlBlock *thread = pending;
    pending = pending->reap_next;
    if (thread->stack_mem)
      kfree(thread->stack_mem);
    kfree(thread);
  }
}

static bool initialize_context(ThreadControlBlock *thread, uint64_t *stack_mem,
                               size_t stack_size, void (*entry)(),
                               ThreadEntry arg_entry, void *argument) {
  if (!thread || !stack_mem ||
      stack_size < sizeof(ThreadContext) + XSTATE_SIZE + 64)
    return false;

  const uint64_t stack_begin = reinterpret_cast<uint64_t>(stack_mem);
  uint64_t stack_top = (stack_begin + stack_size) & ~uint64_t{0xF};

  // iretq does not push a return address. SysV function entry expects RSP=8
  // modulo 16, so make the wrapper's initial stack look like a call entry.
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

extern "C" void thread_start_wrapper(void (*entry)()) {
  entry();
  thread_exit();
}

extern "C" void thread_start_arg_wrapper(ThreadEntry entry, void *argument) {
  entry(argument);
  thread_exit();
}

void scheduler_init() {
  if (scheduler_initialized)
    return;

  // Heap allocation happens before disabling interrupts; the critical section
  // below only publishes fully initialized scheduler state.
  auto *main_tcb =
      static_cast<ThreadControlBlock *>(kmalloc(sizeof(ThreadControlBlock)));
  if (!main_tcb)
    return;

  memset(main_tcb, 0, sizeof(ThreadControlBlock));
  const size_t index = cpu_index();
  memset(&idle_thread_storage[index], 0, sizeof(ThreadControlBlock));
  if (!initialize_context(&idle_thread_storage[index],
                          reinterpret_cast<uint64_t *>(idle_stack_mem[index]),
                          IDLE_STACK_SIZE, idle_loop, nullptr, nullptr)) {
    kfree(main_tcb);
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
  scheduler_initialized = true;
  irq_restore(flags);
}

bool prefer_bootstrap_cpu(uint64_t id) {
  if (!id)
    return false;
  const uint64_t flags = irq_save();
  for (ThreadControlBlock *thread = thread_list_head; thread;
       thread = thread->registry_next) {
    if (thread->id == id && thread->state != ThreadState::TERMINATED) {
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
  ThreadControlBlock *current = cpu_state().current;
  if (!current) {
    irq_restore(flags);
    return {ThreadPriority::NORMAL, UINT32_MAX};
  }
  const KernelIoSchedulingHint previous{current->priority,
                                        current->preferred_cpu};
  if (current != cpu_state().idle) {
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
  if (index >= MAX_APIC_IDS)
    return;
  const uint64_t flags = irq_save();
  CpuSchedulerState &state = cpu_states[index];
  if (!state.current) {
    ThreadControlBlock &idle = idle_thread_storage[index];
    memset(&idle, 0, sizeof(idle));
    if (!initialize_context(&idle,
                            reinterpret_cast<uint64_t *>(idle_stack_mem[index]),
                            IDLE_STACK_SIZE, idle_loop, nullptr, nullptr)) {
      irq_restore(flags);
      return;
    }
    idle.id = 0;
    idle.state = ThreadState::READY;
    idle.priority = ThreadPriority::IDLE;
    state.idle = &idle;
    state.current = &idle;
  }
  irq_restore(flags);
}

static auto create_thread(void (*entry)(), ThreadEntry arg_entry,
                          void *argument, ThreadPriority priority) -> int {
  if (!scheduler_initialized || (!entry && !arg_entry))
    return -1;

  reap_deferred();

  auto *thread =
      static_cast<ThreadControlBlock *>(kmalloc(sizeof(ThreadControlBlock)));
  if (!thread)
    return -1;

  auto *stack_mem = static_cast<uint64_t *>(kmalloc(THREAD_STACK_SIZE));
  if (!stack_mem) {
    kfree(thread);
    return -1;
  }

  memset(thread, 0, sizeof(ThreadControlBlock));
  if (!initialize_context(thread, stack_mem, THREAD_STACK_SIZE, entry,
                          arg_entry, argument)) {
    kfree(stack_mem);
    kfree(thread);
    return -1;
  }

  thread->priority = static_cast<ThreadPriority>(priority_index(priority));
  thread->stop_requested = false;
  thread->wake_tick = 0;
  thread->timed_wait = false;
  thread->sleep_next = nullptr;

  const uint64_t flags = irq_save();
  thread->id = next_pid++;
  register_thread(thread);
  enqueue(thread);
  const int id = static_cast<int>(thread->id);
  irq_restore(flags);
  return id;
}

auto thread_create(void (*entry)(), ThreadPriority priority) -> int {
  return create_thread(entry, nullptr, nullptr, priority);
}

auto thread_create(ThreadEntry entry, void *argument, ThreadPriority priority)
    -> int {
  return create_thread(nullptr, entry, argument, priority);
}

auto current_thread_id() -> uint64_t {
  auto *current_thread = cpu_state().current;
  return current_thread ? current_thread->id : 0;
}

void mark_current_thread_as_application(const char *name) {
  const uint64_t flags = irq_save();
  ThreadControlBlock *current_thread = cpu_state().current;
  if (current_thread) {
    current_thread->application_thread = true;
    current_thread->application_start_ticks = Kernel::Drivers::PIT::get_ticks();
    size_t index = 0;
    if (name) {
      while (name[index] &&
             index + 1 < sizeof(current_thread->application_name)) {
        current_thread->application_name[index] = name[index];
        ++index;
      }
    }
    current_thread->application_name[index] = '\0';
  }
  irq_restore(flags);
}

bool is_current_thread_application() {
  ThreadControlBlock *current_thread = cpu_state().current;
  return current_thread && current_thread->application_thread;
}

size_t get_application_tasks(ApplicationTaskInfo *tasks, size_t capacity) {
  if (!tasks || capacity == 0)
    return 0;

  size_t count = 0;
  const uint64_t flags = irq_save();
  for (ThreadControlBlock *thread = thread_list_head;
       thread && count < capacity; thread = thread->registry_next) {
    if (!thread->application_thread || thread->state == ThreadState::TERMINATED)
      continue;

    ApplicationTaskInfo &task = tasks[count++];
    task.id = thread->id;
    task.start_ticks = thread->application_start_ticks;
    size_t index = 0;
    while (index + 1 < sizeof(task.name) && thread->application_name[index]) {
      task.name[index] = thread->application_name[index];
      ++index;
    }
    task.name[index] = '\0';
  }
  irq_restore(flags);
  return count;
}

void terminate_thread(uint64_t id) {
  const uint64_t flags = irq_save();
  for (ThreadControlBlock *thread = thread_list_head; thread;
       thread = thread->registry_next) {
    if (thread->id != id || thread->state == ThreadState::TERMINATED)
      continue;

    thread->stop_requested = true;
    if (thread->state == ThreadState::BLOCKED) {
      if (thread->timed_wait)
        remove_sleeper(thread);
      else
        remove_blocked(thread);
      thread->timed_wait = false;
      enqueue(thread);
    }
    break;
  }
  irq_restore(flags);
}

auto should_stop() -> bool {
  ThreadControlBlock *current_thread = cpu_state().current;
  return current_thread && current_thread->stop_requested;
}

void thread_yield() { __asm__ volatile("int $0x30" ::: "memory"); }

[[noreturn]] void thread_exit() {
  (void)irq_save();
  ThreadControlBlock *current_thread = cpu_state().current;
  ThreadControlBlock *idle_thread = cpu_state().idle;
  if (current_thread && current_thread != idle_thread) {
    current_thread->state = ThreadState::TERMINATED;
    current_thread->timed_wait = false;
  }

  // The current stack is reclaimed later by reap_deferred(), never here.
  scheduler_lock.clear(std::memory_order_release);
  yield_with_irqs_disabled();
  for (;;)
    __asm__ volatile("hlt" ::: "memory");
}

void thread_block() {
  const uint64_t flags = irq_save();
  ThreadControlBlock *current_thread = cpu_state().current;
  ThreadControlBlock *idle_thread = cpu_state().idle;
  if (!current_thread || current_thread == idle_thread) {
    irq_restore(flags);
    return;
  }

  current_thread->state = ThreadState::BLOCKED;
  current_thread->timed_wait = false;
  insert_blocked(current_thread);
  scheduler_lock.clear(std::memory_order_release);
  yield_with_irqs_disabled();
  irq_restore_flags(flags);
}

auto thread_block_if(bool (*should_block)()) -> bool {
  if (!should_block)
    return false;

  const uint64_t flags = irq_save();
  ThreadControlBlock *current_thread = cpu_state().current;
  ThreadControlBlock *idle_thread = cpu_state().idle;
  if (!current_thread || current_thread == idle_thread || !should_block()) {
    irq_restore(flags);
    return false;
  }

  current_thread->state = ThreadState::BLOCKED;
  current_thread->timed_wait = false;
  insert_blocked(current_thread);
  scheduler_lock.clear(std::memory_order_release);
  yield_with_irqs_disabled();
  irq_restore_flags(flags);
  return true;
}

void thread_sleep_ms(uint32_t milliseconds) {
  if (milliseconds == 0)
    milliseconds = 1;
  milliseconds = std::min(milliseconds, MAX_SLEEP_MS);

  const uint64_t flags = irq_save();
  ThreadControlBlock *current_thread = cpu_state().current;
  ThreadControlBlock *idle_thread = cpu_state().idle;
  if (!current_thread || current_thread == idle_thread) {
    irq_restore(flags);
    return;
  }

  const uint32_t now = static_cast<uint32_t>(Kernel::Drivers::PIT::get_ticks());
  current_thread->wake_tick = now + milliseconds;
  current_thread->timed_wait = true;
  insert_sleeper(current_thread);
  current_thread->state = ThreadState::BLOCKED;

  scheduler_lock.clear(std::memory_order_release);
  yield_with_irqs_disabled();
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
  for (ThreadControlBlock *thread = blocked_list_head; thread;
       thread = thread->sleep_next) {
    if (thread->id == id) {
      remove_blocked(thread);
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
  const uint64_t flags = irq_save();
  CpuSchedulerState &state = cpu_state();
  ThreadControlBlock *current_thread = state.current;
  ThreadControlBlock *idle_thread = state.idle;
  ThreadControlBlock *previous = current_thread;
  if (!previous) {
    irq_restore(flags);
    return current_rsp;
  }

  previous->rsp = current_rsp;
  wake_sleepers(static_cast<uint32_t>(Kernel::Drivers::PIT::get_ticks()));

  const bool previous_is_idle = previous == idle_thread;
  if (!previous_is_idle) {
    if (previous->state == ThreadState::RUNNING) {
      if (ready_priorities == 0) {
        irq_restore(flags);
        return previous->rsp;
      }

      const unsigned p_idx = priority_index(previous->priority);
      RunQueue &queue = run_queues[p_idx];
      if (ready_priorities == (1u << p_idx) && queue.head == previous &&
          queue.tail == previous) {
        irq_restore(flags);
        return previous->rsp;
      }

      enqueue(previous);
    } else if (previous->state == ThreadState::TERMINATED) {
      previous->reap_next = reap_pending;
      reap_pending = previous;
    }
  } else if (ready_priorities == 0) {
    previous->state = ThreadState::RUNNING;
    irq_restore(flags);
    return previous->rsp;
  }

  ThreadControlBlock *next = dequeue_fair();
  if (!next)
    next = idle_thread;

  if (!next) {
    irq_restore(flags);
    if (previous->state == ThreadState::RUNNING)
      return previous->rsp;
    return current_rsp;
  }

  next->state = ThreadState::RUNNING;
  state.current = next;
  const uint64_t next_rsp = next->rsp;
  irq_restore(flags);
  return next_rsp;
}

} // namespace Kernel::Scheduler
