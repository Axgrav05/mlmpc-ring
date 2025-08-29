#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ring {

// 64B cache line padding (common on x86_64); adjust if you profile different HW.
struct CachePad {
    alignas(64) std::byte pad[64];
};

// Memory order helpers for readability
constexpr auto RELAXED = std::memory_order_relaxed;
constexpr auto ACQUIRE = std::memory_order_acquire;
constexpr auto RELEASE = std::memory_order_release;
constexpr auto ACQ_REL = std::memory_order_acq_rel;

// Helpful: round up to next power-of-two (for capacities)
constexpr std::size_t next_pow2(std::size_t x) {
    if (x <= 1) return 1;
    --x;
    x |= x >> 1;  x |= x >> 2;  x |= x >> 4;
    x |= x >> 8;  x |= x >> 16; x |= x >> 32;
    return x + 1;
}

} // namespace ring
