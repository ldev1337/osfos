#pragma once

#include <atomic>
#include <cstdint>

class Spinlock {
private:
  std::atomic_flag m_lock = ATOMIC_FLAG_INIT;

  static auto cpu_save_flags_and_cli() -> uint64_t {
    uint64_t rflags;
#ifdef __x86_64__
    __asm__ volatile("pushfq\n\t"
                     "pop %0\n\t"
                     "cli"
                     : "=r"(rflags)
                     :
                     : "memory");
#endif
    return rflags;
  }

  static void cpu_restore_flags(uint64_t rflags) {
#ifdef __x86_64__
    if ((rflags & (1 << 9)) != 0U) {
      __asm__ volatile("sti" : : : "memory");
    } else {
      __asm__ volatile("cli" : : : "memory");
    }
#endif
  }

public:
  Spinlock() = default;
  ~Spinlock() = default;

  Spinlock(const Spinlock &) = delete;
  auto operator=(const Spinlock &) -> Spinlock & = delete;

  void lock() {
    while (m_lock.test_and_set(std::memory_order_acquire)) {
#ifdef __x86_64__
      __builtin_ia32_pause();
#endif
    }
  }

  auto lock_irqsave() -> uint64_t {
    uint64_t flags = cpu_save_flags_and_cli();
    lock();
    return flags;
  }

  void unlock() { m_lock.clear(std::memory_order_release); }

  void unlock_irqrestore(uint64_t flags) {
    unlock();
    cpu_restore_flags(flags);
  }
};

class SpinlockGuard {
private:
  Spinlock &m_spinlock;
  uint64_t m_flags;

public:
  explicit SpinlockGuard(Spinlock &lock) : m_spinlock(lock) {
    m_flags = m_spinlock.lock_irqsave();
  }

  ~SpinlockGuard() { m_spinlock.unlock_irqrestore(m_flags); }

  SpinlockGuard(const SpinlockGuard &) = delete;
  auto operator=(const SpinlockGuard &) -> SpinlockGuard & = delete;
};