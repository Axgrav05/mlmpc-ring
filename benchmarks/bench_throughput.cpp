// benchmarks/bench_throughput.cpp — MSVC19-friendly, stress-mode exit fix

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "ring/ring_mpmc.hpp"

#if defined(_WIN32)
  // Prevent windows.h from defining min/max macros that break std::min/std::max
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
  #include <immintrin.h>
  static inline void pause_hint() { _mm_pause(); }
  static inline void pin_to_core(DWORD core_index) {
      DWORD_PTR mask = 1ull << (core_index % 64);
      SetThreadAffinityMask(GetCurrentThread(), mask);
  }
#else
  static inline void pause_hint() {}
  static inline void pin_to_core(unsigned) {}
#endif

using SteadyClock = std::chrono::steady_clock;

static std::uint64_t parse_u64(const char* s, std::uint64_t def) {
    if (!s) return def;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    return (end && *end == '\0') ? static_cast<std::uint64_t>(v) : def;
}

int main(int argc, char** argv) {
    // Args: [items_per_producer] [num_producers] [num_consumers] [queue_capacity] [batch] [minutes]
    const std::uint64_t ITEMS_PER_PRODUCER = parse_u64(argc > 1 ? argv[1] : nullptr, 1'000'000ULL);
    const int           NUM_PRODUCERS      = static_cast<int>(parse_u64(argc > 2 ? argv[2] : nullptr, 2));
    const int           NUM_CONSUMERS      = static_cast<int>(parse_u64(argc > 3 ? argv[3] : nullptr, 2));
    const std::uint64_t CAPACITY           = parse_u64(argc > 4 ? argv[4] : nullptr, 1ULL << 14);
    int                 BATCH              = static_cast<int>(parse_u64(argc > 5 ? argv[5] : nullptr, 32));
    const std::uint64_t MINUTES            = parse_u64(argc > 6 ? argv[6] : nullptr, 0);

    std::cout << "Benchmark config:\n"
              << "  items_per_producer = " << ITEMS_PER_PRODUCER << "\n"
              << "  producers          = " << NUM_PRODUCERS << "\n"
              << "  consumers          = " << NUM_CONSUMERS << "\n"
              << "  queue_capacity     = " << CAPACITY << "\n"
              << "  batch              = " << BATCH << "\n"
              << "  minutes (0=finite) = " << MINUTES << "\n";

    ring::RingMPMC<std::uint32_t> q(static_cast<std::size_t>(CAPACITY));

    std::atomic<bool> go{false};
    std::atomic<int>  producers_done{0};     // <-- NEW: producer completion counter

    const std::uint64_t TOTAL_ITEMS = ITEMS_PER_PRODUCER * static_cast<std::uint64_t>(NUM_PRODUCERS);
    std::atomic<std::uint64_t> consumed{0};

    // Latency reservoir (ns) for p50/p95/p99
    std::atomic<uint64_t> lat_samples_count{0};
    constexpr size_t LAT_RESERVOIR = 4096;
    static std::vector<uint32_t> lat_ns; lat_ns.resize(LAT_RESERVOIR);

    // -------- Producers --------
    std::vector<std::thread> producers;
    producers.reserve(NUM_PRODUCERS);
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p] {
            pin_to_core(static_cast<DWORD>(p));
            while (!go.load(std::memory_order_acquire)) {}

            const std::uint64_t base = static_cast<std::uint64_t>(p) * ITEMS_PER_PRODUCER;
            std::vector<std::uint32_t> buf; buf.reserve(static_cast<size_t>(BATCH));
            std::size_t placed = 0;
            int adaptive_batch = BATCH;                   // adaptive producer batch
            const int MIN_B = 8, MAX_B = 256;

            auto flush = [&]() {
                while (placed < buf.size()) {
                    const std::size_t want = buf.size() - placed;
                    const std::size_t did  = q.enqueue_many(buf.data() + placed, want);
                    placed += did;
                    if (placed < buf.size()) pause_hint();
                }
                buf.clear();
                placed = 0;
            };

            auto adapt = [&](std::size_t requested, std::size_t did) {
                if (did < requested)       adaptive_batch = std::min(adaptive_batch * 2, MAX_B); // grow fast
                else if (adaptive_batch > MIN_B) adaptive_batch -= 1;                             // decay slow
            };

            if (MINUTES == 0) {
                for (std::uint64_t i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                    buf.push_back(static_cast<std::uint32_t>(base + i));
                    if (static_cast<int>(buf.size()) == adaptive_batch) {
                        std::size_t did = q.enqueue_many(buf.data(), buf.size());
                        adapt(buf.size(), did);
                        if (did < buf.size()) { placed = did; flush(); } else { buf.clear(); }
                    }
                }
                if (!buf.empty()) {
                    std::size_t did = q.enqueue_many(buf.data(), buf.size());
                    adapt(buf.size(), did);
                    placed = did;
                    if (placed < buf.size()) flush(); else buf.clear();
                }
            } else {
                // stress mode: keep producing until 'go' becomes false
                std::uint64_t i = 0;
                while (go.load(std::memory_order_acquire)) {
                    buf.push_back(static_cast<std::uint32_t>(base + (i++)));
                    if (static_cast<int>(buf.size()) == adaptive_batch) {
                        std::size_t did = q.enqueue_many(buf.data(), buf.size());
                        adapt(buf.size(), did);
                        placed = did;
                        if (placed < buf.size()) flush(); else buf.clear();
                    }
                }
                if (!buf.empty()) {
                    (void)q.enqueue_many(buf.data(), buf.size());
                }
            }

            // mark this producer done
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    // -------- Consumers --------
    std::vector<std::thread> consumers;
    consumers.reserve(NUM_CONSUMERS);
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&, c] {
            pin_to_core(static_cast<DWORD>(NUM_PRODUCERS + c));
            while (!go.load(std::memory_order_acquire)) {}

            std::vector<std::uint32_t> outbuf; outbuf.resize(static_cast<size_t>(BATCH));
            uint64_t sample_token = 0;
            int empty_streak = 0;                   // <-- NEW: consecutive empty polls
            const int EMPTY_STREAK_LIMIT = 2000;    //     (tune if needed)

            for (;;) {
                const std::size_t got = q.dequeue_many(outbuf.data(), static_cast<std::size_t>(BATCH));
                if (got) {
                    empty_streak = 0;
                    auto prev = consumed.fetch_add(got, std::memory_order_relaxed) + got;
                    if (MINUTES == 0 && prev >= TOTAL_ITEMS) break;
                    continue;
                }

                // latency sample (attempt single-item occasionally)
                if ((++sample_token & 0x3FFu) == 0u) {
                    std::uint32_t x{};
                    auto t0 = SteadyClock::now();
                    bool ok = q.try_dequeue(x);
                    auto t1 = SteadyClock::now();
                    if (ok) {
                        empty_streak = 0;
                        uint64_t ns = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                        uint64_t idx = lat_samples_count.fetch_add(1, std::memory_order_relaxed);
                        if (idx < LAT_RESERVOIR) lat_ns[static_cast<size_t>(idx)] = static_cast<uint32_t>(ns);

                        auto prev = consumed.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (MINUTES == 0 && prev >= TOTAL_ITEMS) break;
                        continue;
                    }
                }

                // Finite-run termination
                if (MINUTES == 0) {
                    if (consumed.load(std::memory_order_relaxed) >= TOTAL_ITEMS) break;
                } else {
                    // Stress-mode termination: if all producers are done and we’ve seen
                    // the queue empty for a while, we can stop.
                    if (producers_done.load(std::memory_order_acquire) == NUM_PRODUCERS) {
                        if (++empty_streak >= EMPTY_STREAK_LIMIT) break;
                    } else {
                        empty_streak = 0;
                    }
                }

                pause_hint();
            }
        });
    }

    // -------- Start & timing --------
    auto t0 = SteadyClock::now();
    go.store(true, std::memory_order_release);

    std::thread stopper;
    if (MINUTES > 0) {
        stopper = std::thread([&]() {
            std::this_thread::sleep_for(std::chrono::minutes(MINUTES));
            go.store(false, std::memory_order_release);
        });
    }

    for (auto& th : producers) th.join();
    for (auto& th : consumers)  th.join();
    if (stopper.joinable()) stopper.join();
    auto t1 = SteadyClock::now();

    // -------- Results --------
    const size_t nlat = static_cast<size_t>(
        std::min<uint64_t>(lat_samples_count.load(std::memory_order_relaxed), LAT_RESERVOIR));

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double ops  = (MINUTES == 0)
                        ? static_cast<double>(TOTAL_ITEMS)
                        : static_cast<double>(consumed.load(std::memory_order_relaxed));
    const double ops_per_s = (secs > 0.0) ? (ops / secs) : 0.0;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Results:\n"
              << "  elapsed (s): " << secs << "\n"
              << "  total ops :  " << ops << "\n"
              << "  throughput:  " << (ops_per_s / 1e6) << " Mops/s\n";

    if (nlat >= 8) {
        std::vector<uint32_t> v(lat_ns.begin(), lat_ns.begin() + nlat);
        auto pct = [&](double p) -> uint32_t {
            const double idxd = std::clamp((p / 100.0) * (nlat - 1.0), 0.0, static_cast<double>(nlat - 1));
            const size_t k = static_cast<size_t>(idxd);
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return v[k];
        };
        const auto p50 = pct(50), p95 = pct(95), p99 = pct(99);
        std::cout << "  latency p50/p95/p99 (ns): " << p50 << " / " << p95 << " / " << p99 << "\n";
    }

    return 0;
}
