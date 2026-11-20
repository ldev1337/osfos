
#include <kernel/spinlock.h>

namespace {
constexpr uint64_t RFLAGS_IF = 1ull << 9;

inline auto save_flags_and_cli() noexcept -> uint64_t {
  uint64_t flags;
  __asm__ volatile("pushfq\n\t"
                   "pop %0\n\t"
                   "cli"
                   : "=r"(flags)
                   :
                   : "memory");
  return flags;
}

inline void irq_disable() noexcept { __asm__ volatile("cli" : : : "memory"); }

inline void restore_flags(uint64_t flags) noexcept {
  if (flags & RFLAGS_IF)
    __asm__ volatile("sti" : : : "memory");
}

inline void cpu_relax() noexcept { __builtin_ia32_pause(); }
} // namespace

inline void wait_unlocked(const std::atomic<bool> &locked) noexcept {
  while (locked.load(std::memory_order_relaxed))
    cpu_relax();
}

__attribute__((noinline, cold)) static void
lock_slow(std::atomic<bool> &locked) noexcept {
  for (;;) {
    wait_unlocked(locked);
    if (!locked.exchange(true, std::memory_order_acquire))
      return;
  }
}

__attribute__((noinline, cold)) static auto
lock_irqsave_slow(std::atomic<bool> &locked, uint64_t flags) noexcept
    -> uint64_t {
  for (;;) {
    restore_flags(flags);
    wait_unlocked(locked);
    irq_disable();
    if (!locked.exchange(true, std::memory_order_acquire))
      return flags;
  }
}

void Spinlock::lock() noexcept {
  if (__builtin_expect(!m_locked.exchange(true, std::memory_order_acquire), 1))
    return;
  lock_slow(m_locked);
}

auto Spinlock::try_lock() noexcept -> bool {
  return !m_locked.load(std::memory_order_relaxed) &&
         !m_locked.exchange(true, std::memory_order_acquire);
}

auto Spinlock::is_locked() const noexcept -> bool {
  return m_locked.load(std::memory_order_relaxed);
}

auto Spinlock::lock_irqsave() noexcept -> uint64_t {
  const uint64_t flags = save_flags_and_cli();
  if (__builtin_expect(!m_locked.exchange(true, std::memory_order_acquire), 1))
    return flags;
  return lock_irqsave_slow(m_locked, flags);
}

void Spinlock::unlock() noexcept {
  m_locked.store(false, std::memory_order_release);
}

void Spinlock::unlock_irqrestore(uint64_t flags) noexcept {
  unlock();
  restore_flags(flags);
}
