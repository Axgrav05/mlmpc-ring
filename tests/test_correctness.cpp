#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

#include "../include/ring/ring_spsc.hpp"
#include "../include/ring/ring_mpmc.hpp"

using namespace std::chrono_literals;

template <class Q>
void spsc_smoke() {
    Q q(1024);
    int x = 42;
    bool enq = q.try_enqueue(x);
    int y = -1;
    bool deq = q.try_dequeue(y);
    // With stubs this will be false/false; later you’ll flip these to true.
    (void)enq; (void)deq; (void)y;
    std::cout << "SPSC smoke ran\n";
}

template <class Q>
void mpmc_smoke() {
    Q q(1024);
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> produced{0}, consumed{0};

    std::thread prod([&]{
        while (!go.load()) {}
        for (uint64_t i = 0; i < 1000; ++i) {
            while (!q.try_enqueue(static_cast<int>(i))) {
                std::this_thread::yield();
            }
            produced.fetch_add(1);
        }
    });

    std::thread cons([&]{
        while (!go.load()) {}
        int v;
        for (uint64_t i = 0; i < 1000; ++i) {
            while (!q.try_dequeue(v)) {
                std::this_thread::yield();
            }
            consumed.fetch_add(1);
        }
    });

    go.store(true);
    prod.join();
    cons.join();

    std::cout << "MPMC smoke ran (produced=" << produced.load()
              << ", consumed=" << consumed.load() << ")\n";
}

int main() {
    spsc_smoke<ring::RingSPSC<int>>();
    mpmc_smoke<ring::RingMPMC<int>>();
    std::cout << "Correctness skeleton OK\n";
    return 0;
}
