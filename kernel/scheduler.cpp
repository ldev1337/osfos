#include <kernel/scheduler.h>
#include <kernel/drivers/pit.h>
#include <kernel/memory/heap.h>
#include <arch/x86_64/string.h>

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
    ThreadControlBlock* head;
    ThreadControlBlock* tail;
};

static constexpr unsigned PRIORITY_COUNT = 3;
static constexpr size_t THREAD_STACK_SIZE = 256 * 1024;
static RunQueue run_queues[PRIORITY_COUNT] = {};
static uint32_t ready_priorities = 0;
static unsigned consecutive_high_dispatches = 0;
static unsigned dispatches_since_idle = 0;
static ThreadControlBlock* current_thread = nullptr;
static ThreadControlBlock* idle_thread = nullptr;
static ThreadControlBlock* thread_list_head = nullptr;
static ThreadControlBlock* reap_pending = nullptr;
static ThreadControlBlock* sleep_queue = nullptr;
static uint64_t next_pid = 1;

extern "C" void thread_start_wrapper(void (*)());
extern "C" void thread_start_arg_wrapper(ThreadEntry, void*);

static inline uint64_t irq_save() {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & (1ull << 9))
        __asm__ volatile("sti" : : : "memory");
}

static inline unsigned priority_index(ThreadPriority priority) {
    unsigned index = static_cast<unsigned>(priority);
    return index < PRIORITY_COUNT ? index : static_cast<unsigned>(ThreadPriority::NORMAL);
}

static void enqueue(ThreadControlBlock* thread) {
    unsigned index = priority_index(thread->priority);
    RunQueue& queue = run_queues[index];
    thread->next = nullptr;
    thread->state = ThreadState::READY;
    if (queue.tail)
        queue.tail->next = thread;
    else
        queue.head = thread;
    queue.tail = thread;
    ready_priorities |= 1u << index;
}

static void insert_sleeper(ThreadControlBlock* thread) {
    ThreadControlBlock** link = &sleep_queue;
    while (*link && static_cast<int32_t>((*link)->wake_tick - thread->wake_tick) <= 0)
        link = &(*link)->sleep_next;
    thread->sleep_next = *link;
    *link = thread;
}

static void wake_sleepers(uint32_t now) {
    while (sleep_queue && static_cast<int32_t>(now - sleep_queue->wake_tick) >= 0) {
        ThreadControlBlock* thread = sleep_queue;
        sleep_queue = thread->sleep_next;
        thread->sleep_next = nullptr;
        thread->timed_wait = false;
        enqueue(thread);
    }
}

static ThreadControlBlock* dequeue_priority(unsigned index) {
    if (!(ready_priorities & (1u << index)))
        return nullptr;
    RunQueue& queue = run_queues[index];
    ThreadControlBlock* thread = queue.head;
    queue.head = thread->next;
    if (!queue.head) {
        queue.tail = nullptr;
        ready_priorities &= ~(1u << index);
    }
    thread->next = nullptr;
    return thread;
}

static ThreadControlBlock* dequeue_fair() {
    constexpr unsigned HIGH = static_cast<unsigned>(ThreadPriority::HIGH);
    constexpr unsigned NORMAL = static_cast<unsigned>(ThreadPriority::NORMAL);
    constexpr unsigned IDLE = static_cast<unsigned>(ThreadPriority::IDLE);
    const bool high_ready = (ready_priorities & (1u << HIGH)) != 0;
    const bool normal_ready = (ready_priorities & (1u << NORMAL)) != 0;
    const bool idle_ready = (ready_priorities & (1u << IDLE)) != 0;

    // Even explicitly created idle-priority work receives a bounded share.
    if (idle_ready && (high_ready || normal_ready) && dispatches_since_idle >= 63) {
        dispatches_since_idle = 0;
        consecutive_high_dispatches = 0;
        return dequeue_priority(IDLE);
    }

    // Bound normal-thread starvation under a continuously busy high queue.
    if (high_ready && normal_ready && consecutive_high_dispatches >= 7) {
        consecutive_high_dispatches = 0;
        if (idle_ready)
            ++dispatches_since_idle;
        return dequeue_priority(NORMAL);
    }
    if (high_ready) {
        if (consecutive_high_dispatches < 7)
            ++consecutive_high_dispatches;
        if (idle_ready)
            ++dispatches_since_idle;
        else
            dispatches_since_idle = 0;
        return dequeue_priority(HIGH);
    }
    consecutive_high_dispatches = 0;
    if (normal_ready) {
        if (idle_ready)
            ++dispatches_since_idle;
        else
            dispatches_since_idle = 0;
        return dequeue_priority(NORMAL);
    }
    dispatches_since_idle = 0;
    return dequeue_priority(IDLE);
}

static void register_thread(ThreadControlBlock* thread) {
    thread->registry_prev = nullptr;
    thread->registry_next = thread_list_head;
    if (thread_list_head)
        thread_list_head->registry_prev = thread;
    thread_list_head = thread;
}

static void unlink_thread(ThreadControlBlock* thread) {
    if (thread->registry_prev)
        thread->registry_prev->registry_next = thread->registry_next;
    else
        thread_list_head = thread->registry_next;
    if (thread->registry_next)
        thread->registry_next->registry_prev = thread->registry_prev;
}

static void reap_deferred() {
    uint64_t flags = irq_save();
    ThreadControlBlock* pending = reap_pending;
    reap_pending = nullptr;
    for (ThreadControlBlock* thread = pending; thread; thread = thread->reap_next)
        unlink_thread(thread);
    irq_restore(flags);
    while (pending) {
        ThreadControlBlock* thread = pending;
        pending = pending->reap_next;
        if (thread->stack_mem)
            kfree(thread->stack_mem);
        kfree(thread);
    }
}

static void initialize_context(ThreadControlBlock* thread, uint64_t* stack_mem,
                               void (*entry)(), ThreadEntry arg_entry, void* argument) {
    // A context is restored with iretq, so there is no call instruction to
    // push a return address before the thread entry wrapper runs.  Make the
    // wrapper's incoming RSP look like a normal SysV function entry (8 mod
    // 16); it will then have a 16-byte aligned stack before calling the app.
    uint64_t stack_top = (reinterpret_cast<uint64_t>(stack_mem) + THREAD_STACK_SIZE) & ~uint64_t(0xF);
    if ((stack_top & 0xF) == 0)
        stack_top -= sizeof(uint64_t);
    ThreadContext* context = reinterpret_cast<ThreadContext*>(stack_top - sizeof(ThreadContext));
    memset(context, 0, sizeof(ThreadContext));
    uint64_t xstate_addr = (reinterpret_cast<uint64_t>(context) - 832) & ~uint64_t(0x3F);
    memset(reinterpret_cast<void*>(xstate_addr), 0, 832);
    *reinterpret_cast<uint16_t*>(xstate_addr) = 0x037F;
    *reinterpret_cast<uint32_t*>(xstate_addr + 24) = 0x1F80;
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
}

extern "C" void thread_start_wrapper(void (*entry)()) {
    entry();
    thread_exit();
}

extern "C" void thread_start_arg_wrapper(ThreadEntry entry, void* argument) {
    entry(argument);
    thread_exit();
}

void scheduler_init() {
    uint64_t flags = irq_save();
    ThreadControlBlock* main_tcb = static_cast<ThreadControlBlock*>(kmalloc(sizeof(ThreadControlBlock)));
    if (!main_tcb) {
        irq_restore(flags);
        return;
    }
    main_tcb->id = next_pid++;
    main_tcb->state = ThreadState::RUNNING;
    main_tcb->priority = ThreadPriority::IDLE;
    main_tcb->stack_mem = nullptr;
    main_tcb->rsp = 0;
    main_tcb->next = nullptr;
    main_tcb->reap_next = nullptr;
    main_tcb->stop_requested = false;
    main_tcb->wake_tick = 0;
    main_tcb->timed_wait = false;
    main_tcb->sleep_next = nullptr;
    register_thread(main_tcb);
    idle_thread = main_tcb;
    current_thread = main_tcb;
    irq_restore(flags);
}

static int create_thread(void (*entry)(), ThreadEntry arg_entry, void* argument,
                         ThreadPriority priority) {
    reap_deferred();
    ThreadControlBlock* thread = static_cast<ThreadControlBlock*>(kmalloc(sizeof(ThreadControlBlock)));
    if (!thread)
        return -1;
    uint64_t* stack_mem = static_cast<uint64_t*>(kmalloc(THREAD_STACK_SIZE));
    if (!stack_mem) {
        kfree(thread);
        return -1;
    }

    initialize_context(thread, stack_mem, entry, arg_entry, argument);
    thread->priority = priority;
    thread->stop_requested = false;
    thread->wake_tick = 0;
    thread->timed_wait = false;
    thread->sleep_next = nullptr;

    uint64_t flags = irq_save();
    thread->id = next_pid++;
    register_thread(thread);
    enqueue(thread);
    int id = static_cast<int>(thread->id);
    irq_restore(flags);
    return id;
}

int thread_create(void (*entry)(), ThreadPriority priority) {
    return create_thread(entry, nullptr, nullptr, priority);
}

int thread_create(ThreadEntry entry, void* argument, ThreadPriority priority) {
    return create_thread(nullptr, entry, argument, priority);
}

uint64_t current_thread_id() {
    return current_thread ? current_thread->id : 0;
}

void terminate_thread(uint64_t id) {
    uint64_t flags = irq_save();
    for (ThreadControlBlock* thread = thread_list_head; thread; thread = thread->registry_next) {
        if (thread->id == id && thread->state != ThreadState::TERMINATED) {
            thread->stop_requested = true;
            break;
        }
    }
    irq_restore(flags);
}

bool should_stop() {
    return current_thread && current_thread->stop_requested;
}

void thread_yield() {
    __asm__ volatile("int $0x30" ::: "memory");
}

void thread_exit() {
    uint64_t flags = irq_save();
    if (current_thread) {
        current_thread->state = ThreadState::TERMINATED;
        current_thread->timed_wait = false;
    }
    irq_restore(flags);
    thread_yield();
    for (;;) __asm__ volatile("hlt");
}

void thread_block() {
    uint64_t flags = irq_save();
    if (current_thread) {
        current_thread->state = ThreadState::BLOCKED;
        current_thread->timed_wait = false;
    }
    irq_restore(flags);
    thread_yield();
}

bool thread_block_if(bool (*should_block)()) {
    if (!should_block)
        return false;
    uint64_t flags = irq_save();
    if (!should_block()) {
        irq_restore(flags);
        return false;
    }
    if (current_thread) {
        current_thread->state = ThreadState::BLOCKED;
        current_thread->timed_wait = false;
    }
    irq_restore(flags);
    thread_yield();
    return true;
}

void thread_sleep_ms(uint32_t milliseconds) {
    if (milliseconds == 0)
        milliseconds = 1;
    uint64_t flags = irq_save();
    if (current_thread) {
        if (milliseconds > 0x7FFFFFFFu)
            milliseconds = 0x7FFFFFFFu;
        current_thread->wake_tick = Kernel::Drivers::PIT::get_ticks() + milliseconds;
        current_thread->timed_wait = true;
        insert_sleeper(current_thread);
        current_thread->state = ThreadState::BLOCKED;
    }
    irq_restore(flags);
    thread_yield();
}

void unblock_io_threads() {
    uint64_t flags = irq_save();
    for (ThreadControlBlock* thread = thread_list_head; thread; thread = thread->registry_next) {
        if (thread->state == ThreadState::BLOCKED)
            if (!thread->timed_wait)
                enqueue(thread);
    }
    irq_restore(flags);
}

bool should_preempt() {
    return true;
}

uint64_t schedule(uint64_t current_rsp) {
    if (!current_thread)
        return current_rsp;

    current_thread->rsp = current_rsp;
    wake_sleepers(Kernel::Drivers::PIT::get_ticks());
    ThreadControlBlock* previous = current_thread;
    if (previous->state == ThreadState::RUNNING) {
        if (!ready_priorities)
            return previous->rsp;
        enqueue(previous);
    }
    else if (previous->state == ThreadState::TERMINATED) {
        previous->reap_next = reap_pending;
        reap_pending = previous;
    }

    ThreadControlBlock* next = dequeue_fair();
    if (next == previous) {
        previous->state = ThreadState::RUNNING;
        current_thread = previous;
        return previous->rsp;
    }
    if (!next) {
        if (previous->state == ThreadState::READY) {
            previous->state = ThreadState::RUNNING;
            current_thread = previous;
            return previous->rsp;
        }
        if (previous->state != ThreadState::RUNNING && idle_thread && idle_thread != previous) {
            idle_thread->state = ThreadState::RUNNING;
            current_thread = idle_thread;
            return idle_thread->rsp;
        }
        previous->state = ThreadState::RUNNING;
        current_thread = previous;
        return previous->rsp;
    }

    next->state = ThreadState::RUNNING;
    current_thread = next;
    return next->rsp;
}

}
