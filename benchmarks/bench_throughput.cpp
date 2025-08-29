#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <immintrin.h>
#include "ring/ring_mpmc.hpp"

#ifdef _WIN32
  #include <windows.h>
  static void pin_to_core(DWORD core_index) {
      DWORD_PTR mask = 1ull << (core_index % 64);
      SetThreadAffinityMask(GetCurrentThread(), mask);
  }
#else
  static void pin_to_core(unsigned core_index) { (void)core_index; }
#endif

using SteadyClock = std::chrono::steady_clock;

static std::uint64_t parse_u64(const char* s, std::uint64_t def) {
    if (!s) return def;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    return (end && *end == '\0') ? static_cast<std::uint64_t>(v) : def;
}

int main(int argc, char** argv) {
    // Args: [items_per_producer] [num_producers] [num_consumers] [queue_capacity] [batch]
    const std::uint64_t ITEMS_PER_PRODUCER = parse_u64(argc > 1 ? argv[1] : nullptr, 1'000'000ULL);
    const int           NUM_PRODUCERS      = static_cast<int>(parse_u64(argc > 2 ? argv[2] : nullptr, 2));
    const int           NUM_CONSUMERS      = static_cast<int>(parse_u64(argc > 3 ? argv[3] : nullptr, 2));
    const std::uint64_t CAPACITY           = parse_u64(argc > 4 ? argv[4] : nullptr, 1ULL << 14); // 16384
    const int           BATCH              = static_cast<int>(parse_u64(argc > 5 ? argv[5] : nullptr, 32));

    std::cout << "Benchmark config:\n"
              << "  items_per_producer = " << ITEMS_PER_PRODUCER << "\n"
              << "  producers          = " << NUM_PRODUCERS << "\n"
              << "  consumers          = " << NUM_CONSUMERS << "\n"
              << "  queue_capacity     = " << CAPACITY << "\n"
              << "  batch              = " << BATCH << "\n";

    ring::RingMPMC<std::uint32_t> q(static_cast<std::size_t>(CAPACITY));

    std::atomic<bool> go{false};
    const std::uint64_t TOTAL_ITEMS = ITEMS_PER_PRODUCER * static_cast<std::uint64_t>(NUM_PRODUCERS);
    std::atomic<std::uint64_t> consumed{0};

    // Producers
    std::vector<std::thread> producers;
    producers.reserve(NUM_PRODUCERS);
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p] {
            pin_to_core(p);
            while (!go.load(std::memory_order_acquire)) {}

            const std::uint64_t base = static_cast<std::uint64_t>(p) * ITEMS_PER_PRODUCER;
            std::vector<std::uint32_t> buf; buf.reserve(BATCH);
            std::size_t placed = 0;

            auto flush = [&](bool force=false) {
                // try to place buf[placed..] in chunks until done
                while (placed < buf.size()) {
                    const std::size_t want = buf.size() - placed;
                    const std::size_t did  = q.enqueue_many(buf.data() + placed, want);
                    placed += did;
                    if (placed < buf.size()) { _mm_pause(); }
                }
                if (force) { buf.clear(); placed = 0; }
            };

            for (std::uint64_t i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                buf.push_back(static_cast<std::uint32_t>(base + i));
                if (static_cast<int>(buf.size()) == BATCH) {
                    placed = 0;
                    flush(/*force=*/true);
                }
            }
            // flush remainder
            if (!buf.empty()) {
                placed = 0;
                flush(/*force=*/true);
            }
        });
    }

    // Consumers
    std::vector<std::thread> consumers;
    consumers.reserve(NUM_CONSUMERS);
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&, c] {
            pin_to_core(NUM_PRODUCERS + c);
            while (!go.load(std::memory_order_acquire)) {}

            std::vector<std::uint32_t> outbuf; outbuf.resize(BATCH);

            for (;;) {
                // Try to drain up to BATCH items at once
                const std::size_t got = q.dequeue_many(outbuf.data(), static_cast<std::size_t>(BATCH));
                if (got) {
                    auto prev = consumed.fetch_add(got, std::memory_order_relaxed) + got;
                    if (prev >= TOTAL_ITEMS) break;
                } else {
                    if (consumed.load(std::memory_order_relaxed) >= TOTAL_ITEMS) break;
                    _mm_pause();
                }
            }
        });
    }

    // Start & time
    auto t0 = SteadyClock::now();
    go.store(true, std::memory_order_release);

    for (auto& th : producers) th.join();
    for (auto& th : consumers) th.join();
    auto t1 = SteadyClock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double ops  = static_cast<double>(TOTAL_ITEMS);
    const double ops_per_s = ops / secs;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Results:\n"
              << "  elapsed (s): " << secs << "\n"
              << "  total ops :  " << ops << "\n"
              << "  throughput:  " << (ops_per_s / 1e6) << " Mops/s\n";
    return 0;
}
