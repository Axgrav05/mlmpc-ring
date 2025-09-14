#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include "ring/ring_mpmc.hpp"

using u64 = std::uint64_t;
using SteadyClock = std::chrono::steady_clock;

struct Cfg {
    u64 items_per_producer = 1'000'000;
    int producers = 4;
    int consumers = 4;
    std::size_t capacity = 1u << 16;
    int batch = 32;
};

static u64 parse_u64(const char* s, u64 def) {
    if (!s) return def;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    return (end && *end == '\0') ? static_cast<u64>(v) : def;
}

int main(int argc, char** argv) {
    Cfg cfg;
    if (argc > 1) cfg.items_per_producer = parse_u64(argv[1], cfg.items_per_producer);
    if (argc > 2) cfg.producers          = (int)parse_u64(argv[2], cfg.producers);
    if (argc > 3) cfg.consumers          = (int)parse_u64(argv[3], cfg.consumers);
    if (argc > 4) cfg.capacity           = (std::size_t)parse_u64(argv[4], cfg.capacity);
    if (argc > 5) cfg.batch              = (int)parse_u64(argv[5], cfg.batch);

    const u64 TOTAL = cfg.items_per_producer * (u64)cfg.producers;
    std::cout << "Exactly-once test config:\n"
              << "  items_per_producer = " << cfg.items_per_producer << "\n"
              << "  producers          = " << cfg.producers << "\n"
              << "  consumers          = " << cfg.consumers << "\n"
              << "  queue_capacity     = " << cfg.capacity << "\n"
              << "  batch              = " << cfg.batch << "\n";

    ring::RingMPMC<u64> q(cfg.capacity);

    std::vector<std::atomic<uint8_t>> visited(TOTAL);
    for (auto& a : visited) a.store(0, std::memory_order_relaxed);

    std::atomic<bool> go{false};
    std::atomic<u64> consumed{0};

    // Producers
    std::vector<std::thread> producers;
    producers.reserve(cfg.producers);
    for (int p = 0; p < cfg.producers; ++p) {
        producers.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire)) {}
            const u64 base = (u64)p * cfg.items_per_producer;
            std::vector<u64> buf; buf.reserve(64);
            for (u64 i = 0; i < cfg.items_per_producer; ++i) {
                buf.push_back(base + i);
                if (buf.size() == 64) {
                    std::size_t placed = 0;
                    while (placed < buf.size()) {
                        placed += q.enqueue_many(buf.data() + placed, buf.size() - placed);
                        if (placed < buf.size()) std::this_thread::yield();
                    }
                    buf.clear();
                }
            }
            if (!buf.empty()) {
                std::size_t placed = 0;
                while (placed < buf.size()) {
                    placed += q.enqueue_many(buf.data() + placed, buf.size() - placed);
                    if (placed < buf.size()) std::this_thread::yield();
                }
            }
        });
    }

    // Consumers
    std::vector<std::thread> consumers;
    consumers.reserve(cfg.consumers);
    for (int c = 0; c < cfg.consumers; ++c) {
        consumers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {}
            std::vector<u64> out; out.resize((size_t)cfg.batch);
            for (;;) {
                std::size_t got = q.dequeue_many(out.data(), (size_t)cfg.batch);
                if (got == 0) {
                    if (consumed.load(std::memory_order_relaxed) >= TOTAL) break;
                    std::this_thread::yield();
                    continue;
                }
                for (std::size_t i = 0; i < got; ++i) {
                    u64 id = out[i];
                    if (id >= TOTAL) { std::cerr << "ERROR: out-of-range id=" << id << "\n"; std::abort(); }
                    uint8_t prev = visited[(size_t)id].exchange(1, std::memory_order_relaxed);
                    if (prev != 0) { std::cerr << "ERROR: duplicate id=" << id << "\n"; std::abort(); }
                }
                auto prev = consumed.fetch_add((u64)got, std::memory_order_relaxed) + got;
                if (prev >= TOTAL) break;
            }
        });
    }

    auto t0 = SteadyClock::now();
    go.store(true, std::memory_order_release);

    for (auto& th : producers) th.join();
    for (auto& th : consumers)  th.join();
    auto t1 = SteadyClock::now();

    u64 misses = 0;
    for (u64 i = 0; i < TOTAL; ++i) {
        if (visited[(size_t)i].load(std::memory_order_relaxed) != 1) {
            if (++misses <= 10) std::cerr << "Missing id=" << i << "\n";
        }
    }

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "Exactly-once verification:\n"
              << "  total expected  = " << TOTAL << "\n"
              << "  total consumed  = " << consumed.load() << "\n"
              << "  missing         = " << misses << "\n"
              << "  elapsed (s)     = " << std::fixed << std::setprecision(3) << secs << "\n";

    assert(consumed.load() == TOTAL && "Consumed count mismatch");
    assert(misses == 0 && "Missing items detected");

    std::cout << "PASS: exactly-once under MPMC load.\n";
    return 0;
}
