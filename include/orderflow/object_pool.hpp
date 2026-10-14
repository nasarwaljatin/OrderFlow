#pragma once
// Fixed-capacity object pool with an intrusive free list.
//
// Why: `new`/`delete` on the hot path costs 20-100 ns and has unpredictable
// tail latency (lock contention inside malloc, page faults on fresh memory).
// The pool allocates every slot once, up front, and touches each page during
// construction so the matching loop never page-faults.
//
// acquire() and release() are O(1): pop / push on a singly linked free list
// threaded through the unused slots themselves (zero extra memory).
//
// NOT thread-safe by design: only the matching thread owns orders.

#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace orderflow {

template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity)
        : capacity_(capacity), slots_(std::make_unique<Slot[]>(capacity)) {
        // Thread the free list through every slot. This also pre-faults the pages.
        for (std::size_t i = 0; i + 1 < capacity_; ++i) slots_[i].next = &slots_[i + 1];
        if (capacity_ > 0) slots_[capacity_ - 1].next = nullptr;
        free_ = capacity_ > 0 ? &slots_[0] : nullptr;
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Returns nullptr when the pool is exhausted (caller must handle it).
    template <typename... Args>
    [[nodiscard]] T* acquire(Args&&... args) {
        if (free_ == nullptr) return nullptr;
        Slot* s = free_;
        free_ = s->next;
        ++in_use_;
        return ::new (static_cast<void*>(s->storage)) T{std::forward<Args>(args)...};
    }

    void release(T* obj) noexcept {
        obj->~T();
        Slot* s = reinterpret_cast<Slot*>(obj);  // storage is at offset 0 of the union
        s->next = free_;
        free_ = s;
        --in_use_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t in_use() const noexcept { return in_use_; }
    [[nodiscard]] bool empty() const noexcept { return free_ == nullptr; }

private:
    union Slot {
        Slot* next;
        alignas(T) unsigned char storage[sizeof(T)];
        Slot() : next(nullptr) {}
    };

    std::size_t capacity_;
    std::unique_ptr<Slot[]> slots_;
    Slot* free_{nullptr};
    std::size_t in_use_{0};
};

}  // namespace orderflow
