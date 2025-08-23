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
    auto usage = [&](const char* why = nullptr) {
        if (why) std::cerr << why << "\n";
        std::cerr << "Usage: " << (argv && argv[0] ? argv[0] : "prog")
                  << " <seconds> <slotSize> <slotCount>\n"
                  << "   or:  " << (argv && argv[0] ? argv[0] : "prog")
                  << " --drain <slotSize> <slotCount>\n";
    };

    const bool drainMode = (argc >= 2 && std::string(argv[1]) == "--drain");

    std::size_t slotSize = 0, slotCount = 0;
    long long run_secs = 0;

    if (drainMode) {
        if (argc < 4) { usage("Missing args for --drain"); return 2; }
        try {
            slotSize = static_cast<std::size_t>(std::stoull(argv[2]));
            slotCount = static_cast<std::size_t>(std::stoull(argv[3]));
        } catch (...) { usage("Invalid <slotSize> <slotCount>"); return 2; }
    } else {
        if (argc < 4) { usage("Missing args"); return 2; }
        try {
            run_secs = std::stoll(argv[1]);
        } catch (...) { usage("Invalid seconds"); return 2; }
        if (run_secs <= 0) { usage("Seconds must be > 0"); return 2; }
        try {
            slotSize = static_cast<std::size_t>(std::stoull(argv[2]));
            slotCount = static_cast<std::size_t>(std::stoull(argv[3]));
        } catch (...) { usage("Invalid <slotSize> <slotCount>"); return 2; }
    }
    if (slotSize == 0 || slotCount == 0) { usage("slotSize and slotCount must be > 0"); return 2; }
    if (slotCount <= 1) { usage("slotCount must be > 1"); return 2; }

    const std::string name = drainMode ? "capnp-demo-fs-drain" : "capnp-demo-fs";
    capnproto_shm_transport::ShmFixedSlotDuplexTransport::remove(name);
    {
    capnproto_shm_transport::ShmFixedSlotDuplexTransport a{name, slotSize, slotCount, /*isSideA=*/true, /*openOrCreate=*/true, /*truncateOnCreate=*/true};
    capnproto_shm_transport::ShmFixedSlotDuplexTransport b{name, slotSize, slotCount, /*isSideA=*/false};

    if (drainMode) {
        // Pre-fill the A->B ring with as many messages as possible (slotCount - 1)
        const std::size_t to_send = slotCount - 1; // keep one slot empty
        std::vector<uint8_t> payload(slotSize, 0);
        for (std::size_t i = 0; i < to_send; ++i) {
            (void)a.sendSlot(payload.data(), payload.size(), std::chrono::milliseconds{-1});
        }

        // Measure drain time on B
        std::vector<uint8_t> msg;
        auto t0 = clock::now();
        for (std::size_t i = 0; i < to_send; ++i) {
            (void)b.recvSlot(msg, std::chrono::milliseconds{-1});
        }
    auto t1 = clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    double secs = static_cast<double>(us) / 1e6;
    double pps = secs > 0.0 ? (static_cast<double>(to_send) / secs) : 0.0;
    std::cout.setf(std::ios::fixed); std::cout.precision(3);
    std::cout << "drain_us=" << us << " drain_pps=" << pps << std::endl;
    } else {
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> total{0};

        auto start = clock::now();

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
    }
    } // transports scope ends here
    // Now that both transports are destroyed, unlink the SHM segments.
    capnproto_shm_transport::ShmFixedSlotDuplexTransport::remove(name);
    return 0;
}
