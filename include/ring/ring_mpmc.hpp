#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <new>
#include "ring.hpp"
#include "utils.hpp"

namespace ring {

// Multi-Producer / Multi-Consumer ring with ticketed slots.
// Head/tail indices are global atomics; threads reserve indexes via fetch_add.
// Each slot's ticket encodes lifecycle to avoid ABA.
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

    bool try_enqueue(const T& v) noexcept {
        std::uint64_t pos = tail_.load(RELAXED);
        for (;;) {
            Slot<T>& s = slot(pos);
            std::uint64_t seq = s.seq.load(ACQUIRE);
            // Producer expects seq == pos
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                // Attempt to claim this index
                if (tail_.compare_exchange_weak(pos, pos + 1, ACQ_REL, RELAXED)) {
                    // We own slot 'pos'
                    new (s.ptr()) T(v);
                    s.seq.store(pos + 1, RELEASE); // publish
                    return true;
                }
                // CAS failed: 'pos' updated; retry with new 'pos'
                continue;
            } else if (diff < 0) {
                // seq < pos  => queue appears full (consumer hasn't advanced this slot yet)
                return false;
            } else {
                // Another producer already advanced this slot; reload tail and retry
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
                    new (s.ptr()) T(std::move(v));
                    s.seq.store(pos + 1, RELEASE);
                    return true;
                }
                continue;
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
            // Consumer expects seq == pos + 1
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, ACQ_REL, RELAXED)) {
                    T* p = s.ptr();
                    out = std::move(*p);
                    p->~T();
                    // Make slot available for next wrap
                    s.seq.store(pos + capacity_, RELEASE);
                    return true;
                }
                continue;
            } else if (diff < 0) {
                // seq < pos+1 => queue appears empty
                return false;
            } else {
                // Another consumer already advanced; reload head and retry
                pos = head_.load(RELAXED);
            }
        }
    }


    // Reserve up to items.size() slots with ONE fetch_add on tail.
// Then publish sequentially. This may spin briefly if a slot hasn't been freed yet.
    std::size_t enqueue_many(std::span<const T> items) noexcept {
        const std::size_t n = items.size();
        if (n == 0) return 0;
        // Clamp batch size to capacity to avoid pathological large spans
        const std::size_t want = (n > capacity_) ? capacity_ : n;

        std::uint64_t start = tail_.fetch_add(want, ACQ_REL);
        std::size_t done = 0;
        for (std::size_t i = 0; i < want; ++i) {
            const std::uint64_t idx = start + i;
            Slot<T>& s = slot(idx);
            // Wait (briefly) for this slot to become available (seq == idx)
            std::uint64_t expected = idx;
            // short spin; fall back to yield to be polite
            int spins = 0;
            for (;;) {
                std::uint64_t seq = s.seq.load(ACQUIRE);
                if (seq == expected) break;
                if (++spins < 200) { /* optional: _mm_pause(); */ }
                else { std::this_thread::yield(); spins = 0; }
            }
            new (s.ptr()) T(items[i]);
            s.seq.store(idx + 1, RELEASE);
            ++done;
        }
        return done;
    }

    // Reserve up to out.size() items from head with ONE fetch_add.
    // Spins briefly if an element isn't ready yet.
    std::size_t dequeue_many(std::span<T> out) noexcept {
        const std::size_t n = out.size();
        if (n == 0) return 0;
        const std::size_t want = (n > capacity_) ? capacity_ : n;

        std::uint64_t start = head_.fetch_add(want, ACQ_REL);
        std::size_t done = 0;
        for (std::size_t i = 0; i < want; ++i) {
            const std::uint64_t idx = start + i;
            Slot<T>& s = slot(idx);
            const std::uint64_t expected = idx + 1;
            int spins = 0;
            for (;;) {
                std::uint64_t seq = s.seq.load(ACQUIRE);
                if (seq == expected) break;
                if (++spins < 200) { /* optional: _mm_pause(); */ }
                else { std::this_thread::yield(); spins = 0; }
            }
            T* p = s.ptr();
            out[i] = std::move(*p);
            p->~T();
            s.seq.store(idx + capacity_, RELEASE);
            ++done;
        }
        return done;
    }

    // ---- Add to include/ring/ring_mpmc.hpp (inside class RingMPMC<T>) ----

    // Pointer-based enqueue_many (fallback that doesn't need <span>)
    std::size_t enqueue_many(const T* data, std::size_t n) noexcept {
        if (n == 0) return 0;
        // Clamp to capacity just like the span version
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
                if (++spins < 200) { /* optional: _mm_pause(); */ }
                else { std::this_thread::yield(); spins = 0; }
            }
            new (s.ptr()) T(data[i]);
            s.seq.store(idx + 1, RELEASE);
            ++done;
        }
        return done;
    }

    // Pointer-based dequeue_many
    // Non-reserving dequeue_many: attempt up to n items via try_dequeue.
    // Avoids tail-of-run deadlock when no more items will be produced.
    // Non-blocking, safe batched dequeue:
    // - Peeks at head, counts only the *contiguous ready* items
    // - CAS-claims exactly that many
    // - Consumes them
    std::size_t dequeue_many(T* out, std::size_t n) noexcept {
        if (n == 0) return 0;
        n = (n > capacity_) ? capacity_ : n;

        for (;;) {
            // Snapshot the current head
            std::uint64_t start = head_.load(RELAXED);

            // Count how many *consecutive* items are actually ready
            std::size_t ready = 0;
            while (ready < n) {
                const std::uint64_t idx = start + ready;
                Slot<T>& s = slot(idx);
                const std::uint64_t expected = idx + 1;
                if (s.seq.load(ACQUIRE) != expected) break;
                ++ready;
            }

            if (ready == 0) {
                // Nothing ready right now
                return 0;
            }

            // Try to claim [start, start+ready)
            if (head_.compare_exchange_weak(start, start + ready, ACQ_REL, RELAXED)) {
                // We own these 'ready' slots now; consume them
                for (std::size_t i = 0; i < ready; ++i) {
                    const std::uint64_t idx = (start + i);
                    Slot<T>& s = slot(idx);
                    T* p = s.ptr();
                    out[i] = std::move(*p);
                    p->~T();
                    s.seq.store(idx + capacity_, RELEASE); // free slot
                }
                return ready;
            }
            // CAS lost a race—another consumer advanced head; retry
        }
    }



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

    alignas(64) std::atomic<std::uint64_t> head_;
    CachePad _pad1_;
    alignas(64) std::atomic<std::uint64_t> tail_;
    CachePad _pad2_;
};

} // namespace ring
