#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

template<class T, std::size_t N> class SpscRing {
  static_assert(N >= 2 && (N & (N - 1)) == 0);
  static_assert(std::is_nothrow_copy_assignable_v<T>);
  static_assert(std::atomic<std::size_t>::is_always_lock_free);
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
  alignas(64) std::array<T, N> data_{};
public:
  bool try_push(const T &value) noexcept {
    const auto h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) == N) return false;
    data_[h & (N - 1)] = value;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool try_pop(T &value) noexcept {
    const auto t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    value = data_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
};

// Bounded MPMC with lock-free atomics. A paused reservation can block progress:
// this sequence-number algorithm does NOT provide formal lock-free progress.
template<class T, std::size_t N> class MpmcRing {
  static_assert(N >= 2 && (N & (N - 1)) == 0);
  static_assert(std::is_nothrow_copy_assignable_v<T>);
  static_assert(std::atomic<std::size_t>::is_always_lock_free);
  struct alignas(64) Cell { std::atomic<std::size_t> sequence{}; T value{}; };
  std::array<Cell, N> cells_{};
  alignas(64) std::atomic<std::size_t> enqueue_{0};
  alignas(64) std::atomic<std::size_t> dequeue_{0};
public:
  MpmcRing() { for (std::size_t i = 0; i < N; ++i) cells_[i].sequence.store(i); }
  bool try_push(const T &value) noexcept {
    auto pos = enqueue_.load(std::memory_order_relaxed);
    Cell *cell;
    for (;;) {
      cell = &cells_[pos & (N - 1)];
      auto seq = cell->sequence.load(std::memory_order_acquire);
      auto diff = static_cast<std::intptr_t>(seq - pos);
      if (diff == 0) {
        if (enqueue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      } else if (diff < 0) return false;
      else pos = enqueue_.load(std::memory_order_relaxed);
    }
    cell->value = value;
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }
  bool try_pop(T &value) noexcept {
    auto pos = dequeue_.load(std::memory_order_relaxed);
    Cell *cell;
    for (;;) {
      cell = &cells_[pos & (N - 1)];
      auto seq = cell->sequence.load(std::memory_order_acquire);
      auto diff = static_cast<std::intptr_t>(seq - (pos + 1));
      if (diff == 0) {
        if (dequeue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      } else if (diff < 0) return false;
      else pos = dequeue_.load(std::memory_order_relaxed);
    }
    value = cell->value;
    cell->sequence.store(pos + N, std::memory_order_release);
    return true;
  }
};
