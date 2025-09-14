#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <algorithm>
#include <chrono>
#include "ring.hpp"
#include "utils.hpp"

#if defined(_MSC_VER)
  #include <immintrin.h>
  #define RING_PAUSE() _mm_pause()
#else
  #define RING_PAUSE() do {} while(0)
#endif

namespace ring {

// Multi-Producer / Multi-Consumer ring with ticketed slots.
template <class T>
class RingMPMC {
public:
    explicit RingMPMC(std::size_t capacity)
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

    ~RingMPMC() {
        ::operator delete[](slots_);
    }

    RingMPMC(const RingMPMC&) = delete;
    RingMPMC& operator=(const RingMPMC&) = delete;

    std::size_t capacity() const noexcept { return capacity_; }

    // -------- Single-item ops --------
    bool try_enqueue(const T& v) noexcept {
        std::uint64_t pos = tail_.load(RELAXED);
        for (;;) {
            Slot<T>& s = slot(pos);
            std::uint64_t seq = s.seq.load(ACQUIRE);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, ACQ_REL, RELAXED)) {
                    construct_in_slot(s, v);
                    s.seq.store(pos + 1, RELEASE);
                    return true;
                }
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = tail_.load(RELAXED);
            }
        }
    }

    bool try_enqueue(T&& v) noexcept {
        std::uint64_t pos = tail_.load(RELAXED);
        for (;;) {
            Slot<T>& s = slot(pos);
            std::uint64_t seq = s.seq.load(ACQUIRE);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, ACQ_REL, RELAXED)) {
                    construct_in_slot(s, std::move(v));
                    s.seq.store(pos + 1, RELEASE);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = tail_.load(RELAXED);
            }
        }
    }

    bool try_dequeue(T& out) noexcept {
        std::uint64_t pos = head_.load(RELAXED);
        for (;;) {
            Slot<T>& s = slot(pos);
            std::uint64_t seq = s.seq.load(ACQUIRE);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, ACQ_REL, RELAXED)) {
                    move_out_and_destroy(s, out);
                    s.seq.store(pos + capacity_, RELEASE);
                    return true;
                }
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = head_.load(RELAXED);
            }
        }
    }

    // -------- Deadline wrappers (spin/yield until deadline) --------
    template <class Clock, class Dur>
    bool enqueue_until(const T& v, const std::chrono::time_point<Clock, Dur>& deadline) noexcept {
        int spins = 0;
        do {
            if (try_enqueue(v)) return true;
            if (++spins < 200) { RING_PAUSE(); }
            else { std::this_thread::yield(); spins = 0; }
        } while (Clock::now() < deadline);
        return false;
    }

    template <class Clock, class Dur>
    bool dequeue_until(T& out, const std::chrono::time_point<Clock, Dur>& deadline) noexcept {
        int spins = 0;
        do {
            if (try_dequeue(out)) return true;
            if (++spins < 200) { RING_PAUSE(); }
            else { std::this_thread::yield(); spins = 0; }
        } while (Clock::now() < deadline);
        return false;
    }

    // -------- Batched enqueue (block reservation) --------
    // Pointer overload (portable for VS2019)
    std::size_t enqueue_many(const T* data, std::size_t n) noexcept {
        if (n == 0) return 0;
        const std::size_t want = (n > capacity_) ? capacity_ : n;

        std::uint64_t start = tail_.fetch_add(want, ACQ_REL);
        std::size_t done = 0;
        for (std::size_t i = 0; i < want; ++i) {
            const std::uint64_t idx = start + i;
            Slot<T>& s = slot(idx);
            const std::uint64_t expected = idx;

            int spins = 0;
            for (;;) {
                std::uint64_t seq = s.seq.load(ACQUIRE);
                if (seq == expected) break;
                if (++spins < 200) { RING_PAUSE(); }
                else { std::this_thread::yield(); spins = 0; }
            }

            construct_in_slot(s, data[i]);
            s.seq.store(idx + 1, RELEASE);
            ++done;
        }
        return done;
    }

    // -------- Batched dequeue (non-reserving, claim only ready run) --------
    std::size_t dequeue_many(T* out, std::size_t n) noexcept {
        if (n == 0) return 0;
        n = (n > capacity_) ? capacity_ : n;

        for (;;) {
            std::uint64_t start = head_.load(RELAXED);

            // Count contiguous ready items
            std::size_t ready = 0;
            while (ready < n) {
                const std::uint64_t idx = start + ready;
                Slot<T>& s = slot(idx);
                if (s.seq.load(ACQUIRE) != (idx + 1)) break;
                ++ready;
            }
            if (ready == 0) return 0;

            if (head_.compare_exchange_weak(start, start + ready, ACQ_REL, RELAXED)) {
                for (std::size_t i = 0; i < ready; ++i) {
                    const std::uint64_t idx = start + i;
                    Slot<T>& s = slot(idx);
                    move_out_and_destroy(s, out[i]);
                    s.seq.store(idx + capacity_, RELEASE);
                }
                return ready;
            }
            // lost race; retry
        }
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

    Slot<T>& slot(std::uint64_t idx) noexcept {
        return slots_[static_cast<std::size_t>(idx) & mask_];
    }

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
