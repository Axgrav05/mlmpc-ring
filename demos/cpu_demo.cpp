#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>
#include "../include/ring/ring_mpmc.hpp"

int main() {
    ring::RingMPMC<std::vector<int>> q(1024);
    std::atomic<bool> done{false};

    std::thread prod([&]{
        for (int i = 0; i < 10; ++i) {
            std::vector<int> batch(8, i);
            while (!q.try_enqueue(std::move(batch))) {
                std::this_thread::yield();
            }
        }
        done.store(true);
    });

    std::thread cons([&]{
        std::vector<int> batch;
        int drained = 0;
        while (!done.load() || q.size() > 0) {
            if (q.try_dequeue(batch)) {
                drained += static_cast<int>(batch.size());
            } else {
                std::this_thread::yield();
            }
        }
        std::cout << "Drained items: " << drained << "\n";
    });

    prod.join();
    cons.join();
    std::cout << "CPU demo skeleton done\n";
    return 0;
}
