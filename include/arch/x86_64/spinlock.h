#pragma once

#include <atomic>
#include <cstdint>

class Spinlock {
private:
  static constexpr uint64_t RFLAGS_IF = 1ull << 9;

  std::atomic<bool> m_locked{false};

  static inline auto save_flags_and_cli() noexcept -> uint64_t {
    uint64_t rflags = 0;
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

  static inline void irq_disable() noexcept {
#ifdef __x86_64__
    __asm__ volatile("cli" : : : "memory");
#endif
  }

  static inline void restore_flags(uint64_t rflags) noexcept {
#ifdef __x86_64__
    if (rflags & RFLAGS_IF)
      __asm__ volatile("sti" : : : "memory");
#else
    (void)rflags;
#endif
  }

  static inline void cpu_relax() noexcept {
#ifdef __x86_64__
    __builtin_ia32_pause();
#endif
  }

  inline void wait_unlocked() const noexcept {
    while (m_locked.load(std::memory_order_relaxed))
      cpu_relax();
  }

  [[gnu::noinline, gnu::cold]] void lock_slow() noexcept {
    for (;;) {
      wait_unlocked();
      if (!m_locked.exchange(true, std::memory_order_acquire))
        return;
    }
  }

  [[gnu::noinline, gnu::cold]] auto lock_irqsave_slow(uint64_t flags) noexcept
      -> uint64_t {
    for (;;) {
      restore_flags(flags);
      wait_unlocked();
      irq_disable();
      if (!m_locked.exchange(true, std::memory_order_acquire))
        return flags;
    }
  }

public:
  Spinlock() = default;
  ~Spinlock() = default;

  Spinlock(const Spinlock &) = delete;
  auto operator=(const Spinlock &) -> Spinlock & = delete;

  inline void lock() noexcept {
    if (__builtin_expect(!m_locked.exchange(true, std::memory_order_acquire),
                         1))
      return;
    lock_slow();
  }

  inline auto try_lock() noexcept -> bool {
    return !m_locked.load(std::memory_order_relaxed) &&
           !m_locked.exchange(true, std::memory_order_acquire);
  }

  inline auto is_locked() const noexcept -> bool {
    return m_locked.load(std::memory_order_relaxed);
  }

  inline auto lock_irqsave() noexcept -> uint64_t {
    const uint64_t flags = save_flags_and_cli();
    if (__builtin_expect(!m_locked.exchange(true, std::memory_order_acquire),
                         1))
      return flags;
    return lock_irqsave_slow(flags);
  }

  inline void unlock() noexcept {
    m_locked.store(false, std::memory_order_release);
  }

  inline void unlock_irqrestore(uint64_t flags) noexcept {
    unlock();
    restore_flags(flags);
  }
};

class SpinlockGuard {
private:
  Spinlock &m_spinlock;
  uint64_t m_flags;

public:
  explicit SpinlockGuard(Spinlock &lock) noexcept
      : m_spinlock(lock), m_flags(lock.lock_irqsave()) {}

  ~SpinlockGuard() { m_spinlock.unlock_irqrestore(m_flags); }

  SpinlockGuard(const SpinlockGuard &) = delete;
  auto operator=(const SpinlockGuard &) -> SpinlockGuard & = delete;
};