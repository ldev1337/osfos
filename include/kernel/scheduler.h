#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Kernel::Scheduler {

enum class ThreadState {
    READY,
    RUNNING,
    BLOCKED,
    TERMINATED
};

enum class ThreadPriority {
    IDLE = 0,
    NORMAL = 1,
    HIGH = 2
};

struct ThreadControlBlock {
    uint64_t id;
    ThreadState state;
    ThreadPriority priority;
    uint64_t* stack_mem;
    uint64_t rsp;
    ThreadControlBlock* next;
};

void scheduler_init();
int thread_create(void (*entry)(), ThreadPriority priority = ThreadPriority::NORMAL);
void thread_yield();
void thread_exit();
void thread_block();
void unblock_io_threads();
bool should_preempt();
uint64_t schedule(uint64_t current_rsp);

}