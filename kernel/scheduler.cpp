#include <kernel/scheduler.h>
#include <kernel/memory/heap.h>
#include <arch/x86_64/string.h>

extern "C" void kernel_printf(const char* fmt, ...);

namespace Kernel::Scheduler {

struct ThreadContext {
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

static ThreadControlBlock* current_thread = nullptr;
static ThreadControlBlock* thread_list_head = nullptr;
static uint64_t next_pid = 1;
static volatile bool yield_requested = false;

extern "C" void thread_start_wrapper(void (*entry)()) {
    entry();
    thread_exit();
}

void scheduler_init() {
    ThreadControlBlock* main_tcb = (ThreadControlBlock*)kmalloc(sizeof(ThreadControlBlock));
    main_tcb->id = next_pid++;
    main_tcb->state = ThreadState::RUNNING;
    main_tcb->priority = ThreadPriority::IDLE;
    main_tcb->stack_mem = nullptr;
    main_tcb->rsp = 0;
    main_tcb->next = nullptr;

    thread_list_head = main_tcb;
    current_thread = main_tcb;
}

int thread_create(void (*entry)(), ThreadPriority priority) {
    ThreadControlBlock* tcb = (ThreadControlBlock*)kmalloc(sizeof(ThreadControlBlock));
    if (!tcb) return -1;

    size_t stack_size = 8192; // 8KB stack
    uint64_t* stack_mem = (uint64_t*)kmalloc(stack_size);
    if (!stack_mem) {
        kfree(tcb);
        return -1;
    }

    tcb->id = next_pid++;
    tcb->state = ThreadState::READY;
    tcb->priority = priority;
    tcb->stack_mem = stack_mem;

    uint64_t stack_top = (uint64_t)stack_mem + stack_size;
    ThreadContext* ctx = (ThreadContext*)(stack_top - sizeof(ThreadContext) - sizeof(uint64_t));
    memset(ctx, 0, sizeof(ThreadContext));

    ctx->rdi = (uint64_t)entry;
    ctx->rip = (uint64_t)thread_start_wrapper;
    ctx->cs = 0x08;         // Kernel threads run at ring 0; no TSS ring-1 stack is installed.
    ctx->rflags = 0x202;    // IF=1 (interrupts enabled)
    ctx->rsp = stack_top;
    ctx->ss = 0x10;

    tcb->rsp = (uint64_t)ctx;

    tcb->next = nullptr;
    if (!thread_list_head) {
        thread_list_head = tcb;
    } else {
        ThreadControlBlock* curr = thread_list_head;
        while (curr->next) {
            curr = curr->next;
        }
        curr->next = tcb;
    }

    return (int)tcb->id;
}

void thread_yield() {
    yield_requested = true;
    __asm__ volatile("int $0x20");
}

void thread_exit() {
    if (current_thread) {
        current_thread->state = ThreadState::TERMINATED;
    }
    thread_yield();
    while (1) {
        __asm__ volatile("hlt");
    }
}

void thread_block() {
    if (current_thread) {
        current_thread->state = ThreadState::BLOCKED;
    }
}

void unblock_io_threads() {
    for (ThreadControlBlock* t = thread_list_head; t; t = t->next) {
        if (t->state == ThreadState::BLOCKED) {
            t->state = ThreadState::READY;
        }
    }
}

bool should_preempt() {
    if (yield_requested) {
        yield_requested = false;
        return true;
    }
    if (current_thread && current_thread->priority == ThreadPriority::IDLE) {
        for (ThreadControlBlock* t = thread_list_head; t; t = t->next) {
            if (t != current_thread && t->state == ThreadState::READY) {
                return true;
            }
        }
    }
    return false;
}

uint64_t schedule(uint64_t current_rsp) {
    if (!current_thread) return current_rsp;

    current_thread->rsp = current_rsp;
    if (current_thread->state == ThreadState::RUNNING) {
        current_thread->state = ThreadState::READY;
    }

    // Find the highest priority among all currently READY threads
    int highest_priority = -1;
    for (ThreadControlBlock* t = thread_list_head; t; t = t->next) {
        if (t->state == ThreadState::READY) {
            int p = static_cast<int>(t->priority);
            if (p > highest_priority) {
                highest_priority = p;
            }
        }
    }

    if (highest_priority == -1) {
        if (current_thread->state == ThreadState::READY) {
            current_thread->state = ThreadState::RUNNING;
        }
        return current_thread->rsp;
    }

    // Round-robin selection among READY threads of the highest priority level
    ThreadControlBlock* next = current_thread->next ? current_thread->next : thread_list_head;
    ThreadControlBlock* start = next;
    while (true) {
        if (next->state == ThreadState::READY && static_cast<int>(next->priority) == highest_priority) {
            break;
        }
        next = next->next ? next->next : thread_list_head;
        if (next == start) {
            break;
        }
    }

    current_thread = next;
    current_thread->state = ThreadState::RUNNING;
    return current_thread->rsp;
}

}
