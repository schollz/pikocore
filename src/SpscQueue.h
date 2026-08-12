#pragma once

#include <stddef.h>
#include <stdint.h>

// A bounded single-producer/single-consumer queue. The producer owns head and
// the consumer owns tail; the barriers make it suitable for RP2040 IRQ/core
// hand-offs as well as native tests.
template <typename T, size_t Capacity>
class SpscQueue {
  static_assert(Capacity > 1, "queue needs at least two slots");

 public:
  bool push(const T& value) {
    const uint32_t head = head_;
    const uint32_t next = increment(head);
    if (next == tail_) {
      ++drops_;
      return false;
    }
    entries_[head] = value;
    barrier();
    head_ = next;
    return true;
  }

  bool pop(T& value) {
    const uint32_t tail = tail_;
    if (tail == head_) {
      return false;
    }
    value = entries_[tail];
    barrier();
    tail_ = increment(tail);
    return true;
  }

  void clear() {
    tail_ = head_;
    barrier();
  }

  uint32_t drops() const { return drops_; }
  bool empty() const { return head_ == tail_; }

 private:
  static uint32_t increment(uint32_t index) {
    ++index;
    return index == Capacity ? 0u : index;
  }

  static void barrier() {
#if defined(__GNUC__)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
  }

  T entries_[Capacity]{};
  volatile uint32_t head_ = 0;
  volatile uint32_t tail_ = 0;
  volatile uint32_t drops_ = 0;
};
