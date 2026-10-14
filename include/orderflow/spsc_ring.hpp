#pragma once
// Bounded lock-free single-producer / single-consumer ring buffer.
//
// Used to hand Commands from the network thread to the matching thread (and
// Events back) without a mutex. Key ideas, each worth explaining in an interview:
//
//  1. Exactly one writer per index: the producer owns `tail_`, the consumer
//     owns `head_`. No CAS loops are needed, only acquire/release ordering.
//
//  2. Memory ordering: the producer writes the slot, then publishes it with a
//     release-store of `tail_`. The consumer's acquire-load of `tail_`
//     guarantees it sees the slot's contents. Symmetric for `head_`.
//
//  3. False sharing: head_ and tail_ live on separate cache lines
//     (alignas(64)), otherwise every push would invalidate the consumer's line.
//
//  4. Cached indices: each side keeps a private copy of the *other* side's
//     index and only re-reads the shared atomic when the ring looks full/empty.
//     This removes most cross-core cache traffic.
//
//  5. Capacity is a power of two, so `index & mask_` replaces a modulo.

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace orderflow {

inline constexpr std::size_t kCacheLine = 64;

template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing stores PODs by value");

public:
    explicit SpscRing(std::size_t capacity) : capacity_(capacity), mask_(capacity - 1) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("SpscRing capacity must be a power of two >= 2");
        buffer_ = std::make_unique<T[]>(capacity);
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer thread only.
    bool try_push(const T& value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ == capacity_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == capacity_) return false;  // full
        }
        buffer_[tail & mask_] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only.
    bool try_pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return false;  // empty
        }
        out = buffer_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Approximate (racy by nature); fine for monitoring.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<T[]> buffer_;

    // Consumer-owned line: head + consumer's cached copy of tail.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_{0};

    // Producer-owned line: tail + producer's cached copy of head.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_{0};

    // Padding so whatever follows this object doesn't share the producer's line.
    char pad_[kCacheLine - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)]{};
};

}  // namespace orderflow
