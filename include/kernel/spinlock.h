#pragma once

#include <atomic>
#include <cstdint>

class Spinlock {
private:
  std::atomic<bool> m_locked{false};

public:
  Spinlock() noexcept = default;
  ~Spinlock() = default;

  Spinlock(const Spinlock &) = delete;
  auto operator=(const Spinlock &) -> Spinlock & = delete;

  void lock() noexcept;
  auto try_lock() noexcept -> bool;
  auto is_locked() const noexcept -> bool;
  auto lock_irqsave() noexcept -> uint64_t;
  void unlock() noexcept;
  void unlock_irqrestore(uint64_t flags) noexcept;
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
