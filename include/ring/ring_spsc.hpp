#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <new>
#include "ring.hpp"
#include "utils.hpp"

namespace ring {

// Single-Producer / Single-Consumer ring with ticketed slots.
// Fixed capacity (power-of-two recommended).
template <class T>
class RingSPSC {
public:
    explicit RingSPSC(std::size_t capacity)
        : capacity_(next_pow2(capacity)),
          mask_(capacity_ - 1),
          slots_(static_cast<Slot<T>*>(::operator new[](capacity_ * sizeof(Slot<T>)))),
          head_(0), tail_(0)
    {
        // Initialize slot tickets: seq = index
        for (std::size_t i = 0; i < capacity_; ++i) {
            new (&slots_[i]) Slot<T>();
            slots_[i].seq.store(static_cast<std::uint64_t>(i), RELAXED);
        }
    }

    ~RingSPSC() {
        // NOTE: Caller must ensure queue is drained or only trivially-destructible T is stored.
        ::operator delete[](slots_);
    }

    RingSPSC(const RingSPSC&) = delete;
    RingSPSC& operator=(const RingSPSC&) = delete;

    std::size_t capacity() const noexcept { return capacity_; }

    bool try_enqueue(const T& v) noexcept {
    const uint64_t t = tail_.load(RELAXED);
    Slot<T>& s = slot(t);
    // Producer expects seq == t
    if (s.seq.load(ACQUIRE) != t) return false; // full
    // Construct in-place
    new (s.ptr()) T(v);
    // Publish: payload must be visible before seq advances
    s.seq.store(t + 1, RELEASE);
    tail_.store(t + 1, RELAXED);
    return true;
    }

    bool try_enqueue(T&& v) noexcept {
        const uint64_t t = tail_.load(RELAXED);
        Slot<T>& s = slot(t);
        if (s.seq.load(ACQUIRE) != t) return false; // full
        new (s.ptr()) T(std::move(v));
        s.seq.store(t + 1, RELEASE);
        tail_.store(t + 1, RELAXED);
        return true;
    }

    bool try_dequeue(T& out) noexcept {
        const uint64_t h = head_.load(RELAXED);
        Slot<T>& s = slot(h);
        // Consumer expects seq == h + 1
        if (s.seq.load(ACQUIRE) != (h + 1)) return false; // empty
        // Move out, destroy in-place object
        T* p = s.ptr();
        out = std::move(*p);
        p->~T();
        // Make slot available for the next wrap: set to h + capacity_
        s.seq.store(h + capacity_, RELEASE);
        head_.store(h + 1, RELAXED);
        return true;
    }


    // Batch enqueue: returns number actually enqueued (0..n)
    std::size_t enqueue_many(std::span<const T> items) noexcept {
        // TODO: Reserve a run of slots and write with fewer atomics
        (void)items;
        return 0; // stub
    }

    // Batch dequeue: fills 'out' with up to out.size() items; returns count
    std::size_t dequeue_many(std::span<T> out) noexcept {
        (void)out;
        return 0; // stub
    }

    // Approximate size (SPSC: safe snapshot)
    std::size_t size() const noexcept {
        auto h = head_.load(RELAXED);
        auto t = tail_.load(RELAXED);
        return static_cast<std::size_t>(t - h);
    }

private:
    Slot<T>& slot(std::uint64_t idx) noexcept { return slots_[static_cast<std::size_t>(idx) & mask_]; }

    const std::size_t capacity_;
    const std::size_t mask_;
    Slot<T>* slots_;

    // Producer advances tail; Consumer advances head
    alignas(64) std::atomic<std::uint64_t> head_;
    CachePad _pad1_;
    alignas(64) std::atomic<std::uint64_t> tail_;
    CachePad _pad2_;
};

} // namespace ring
