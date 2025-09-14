#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <chrono>
#include <thread>
#include "ring.hpp"
#include "utils.hpp"

namespace ring {

// Single-Producer / Single-Consumer ring with ticketed slots.
template <class T>
class RingSPSC {
public:
    explicit RingSPSC(std::size_t capacity)
        : capacity_(next_pow2(capacity)),
          mask_(capacity_ - 1),
          slots_(static_cast<Slot<T>*>(::operator new[](capacity_ * sizeof(Slot<T>)))),
          head_(0), tail_(0)
    {
        for (std::size_t i = 0; i < capacity_; ++i) {
            new (&slots_[i]) Slot<T>();
            slots_[i].seq.store(static_cast<std::uint64_t>(i), RELAXED);
        }
    }

    ~RingSPSC() { ::operator delete[](slots_); }

    RingSPSC(const RingSPSC&) = delete;
    RingSPSC& operator=(const RingSPSC&) = delete;

    std::size_t capacity() const noexcept { return capacity_; }

    bool try_enqueue(const T& v) noexcept {
        const std::uint64_t t = tail_.load(RELAXED);
        Slot<T>& s = slot(t);
        if (s.seq.load(ACQUIRE) != t) return false; // full
        construct_in_slot(s, v);
        s.seq.store(t + 1, RELEASE);
        tail_.store(t + 1, RELAXED);
        return true;
    }

    bool try_enqueue(T&& v) noexcept {
        const std::uint64_t t = tail_.load(RELAXED);
        Slot<T>& s = slot(t);
        if (s.seq.load(ACQUIRE) != t) return false;
        construct_in_slot(s, std::move(v));
        s.seq.store(t + 1, RELEASE);
        tail_.store(t + 1, RELAXED);
        return true;
    }

    bool try_dequeue(T& out) noexcept {
        const std::uint64_t h = head_.load(RELAXED);
        Slot<T>& s = slot(h);
        if (s.seq.load(ACQUIRE) != (h + 1)) return false; // empty
        move_out_and_destroy(s, out);
        s.seq.store(h + capacity_, RELEASE);
        head_.store(h + 1, RELAXED);
        return true;
    }

    template <class Clock, class Dur>
    bool enqueue_until(const T& v, const std::chrono::time_point<Clock, Dur>& deadline) noexcept {
        do { if (try_enqueue(v)) return true; std::this_thread::yield(); }
        while (Clock::now() < deadline);
        return false;
    }

    template <class Clock, class Dur>
    bool dequeue_until(T& out, const std::chrono::time_point<Clock, Dur>& deadline) noexcept {
        do { if (try_dequeue(out)) return true; std::this_thread::yield(); }
        while (Clock::now() < deadline);
        return false;
    }

    std::size_t size() const noexcept {
        auto h = head_.load(RELAXED);
        auto t = tail_.load(RELAXED);
        return static_cast<std::size_t>(t - h);
    }

private:
    template <class U>
    static inline void construct_in_slot(Slot<T>& s, U&& value) noexcept {
        if constexpr (std::is_trivially_copyable_v<T>) {
            *s.ptr() = static_cast<T>(std::forward<U>(value));
        } else {
            new (s.ptr()) T(std::forward<U>(value));
        }
    }
    static inline void move_out_and_destroy(Slot<T>& s, T& out) noexcept {
        if constexpr (std::is_trivially_copyable_v<T>) {
            out = *s.ptr();
        } else {
            T* p = s.ptr();
            out = std::move(*p);
            p->~T();
        }
    }

    Slot<T>& slot(std::uint64_t idx) noexcept { return slots_[static_cast<std::size_t>(idx) & mask_]; }

private:
    const std::size_t capacity_;
    const std::size_t mask_;
    Slot<T>* slots_;

    alignas(64) std::atomic<std::uint64_t> head_;
    CachePad _pad1_;
    alignas(64) std::atomic<std::uint64_t> tail_;
    CachePad _pad2_;
};

} // namespace ring
