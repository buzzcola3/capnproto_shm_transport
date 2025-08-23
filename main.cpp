// Simple one-way throughput & latency tool
// Measures:
// - messages per second (A -> B)
// - average one-way latency from A send to B receive
#include "capnproto_shm_transport.hpp"
#include <iostream>
#include <thread>
#include <cstring>
#include <vector>
#include <chrono>
#include <cstdint>
#include <string>
#include <atomic>

namespace {
using clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << (argv && argv[0] ? argv[0] : "prog")
                  << " <seconds> <slotSize> <slotCount>\n";
        return 2;
    }
    long long run_secs = 0;
    try {
        run_secs = std::stoll(argv[1]);
    } catch (...) {
        std::cerr << "Invalid seconds: '" << argv[1] << "'\n";
        return 2;
    }
    if (run_secs <= 0) {
        std::cerr << "Seconds must be > 0\n";
        return 2;
    }
    std::size_t slotSize = 0, slotCount = 0;
    try {
        slotSize = static_cast<std::size_t>(std::stoull(argv[2]));
        slotCount = static_cast<std::size_t>(std::stoull(argv[3]));
    } catch (...) {
        std::cerr << "Invalid <slotSize> <slotCount>\n";
        return 2;
    }
    if (slotSize == 0 || slotCount == 0) {
        std::cerr << "slotSize and slotCount must be > 0\n";
        return 2;
    }

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total{0};

    auto start = clock::now();

    const std::string name = "capnp-demo-fs";
    capnproto_shm_transport::ShmFixedSlotDuplexTransport::remove(name);
    capnproto_shm_transport::ShmFixedSlotDuplexTransport a{name, slotSize, slotCount, /*isSideA=*/true, /*openOrCreate=*/true, /*truncateOnCreate=*/true};
    capnproto_shm_transport::ShmFixedSlotDuplexTransport b{name, slotSize, slotCount, /*isSideA=*/false};

    std::thread sender([&] {
        std::vector<uint8_t> payload(slotSize, 0);
        while (!stop.load(std::memory_order_relaxed)) {
            a.sendSlot(payload.data(), payload.size(), std::chrono::milliseconds{-1});
        }
    });

    std::thread receiver([&] {
        std::vector<uint8_t> msg;
        while (!stop.load(std::memory_order_relaxed)) {
            if (!b.recvSlot(msg, std::chrono::milliseconds{200})) continue;
            total.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(run_secs));
    stop.store(true, std::memory_order_relaxed);
    sender.join();
    receiver.join();

    auto end = clock::now();
    double secs = std::chrono::duration<double>(end - start).count();
    uint64_t n = total.load(std::memory_order_relaxed);
    double pps = secs > 0 ? (static_cast<double>(n) / secs) : 0.0;
    std::cout << "pps=" << static_cast<long long>(pps) << std::endl;
    return 0;
}
