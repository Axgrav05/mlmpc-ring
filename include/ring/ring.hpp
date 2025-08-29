#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include "utils.hpp"

namespace ring {

// Invariant (ticketed slot):
//  For slot at logical index i:
//    - Producer expects seq == i
//    - After write, producer sets seq = i + 1
//    - Consumer expects seq == i + 1
//    - After read, consumer sets seq = i + Capacity
//
// This prevents ABA on wrap-around and encodes lifecycle.

template <class T>
struct Slot {
    std::atomic<std::uint64_t> seq; // ticket
    alignas(64) std::aligned_storage_t<sizeof(T), alignof(T)> storage;

    T*       ptr()       noexcept { return std::launder(reinterpret_cast<T*>(&storage)); }
    const T* ptr() const noexcept { return std::launder(reinterpret_cast<const T*>(&storage)); }
};

} // namespace ring
