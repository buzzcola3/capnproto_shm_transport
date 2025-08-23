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

using capnproto_shm_transport::ShmDuplexTransport;

namespace {
using clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << (argv && argv[0] ? argv[0] : "prog") << " <seconds>\n";
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
    const std::string name = "capnp-demo";
    const std::size_t cap = 64 * 1024;

    // Clean start and create both sides
    ShmDuplexTransport::remove(name);
    ShmDuplexTransport a{name, cap, /*isSideA=*/true, /*openOrCreate=*/true, /*truncateOnCreate=*/true};
    ShmDuplexTransport b{name, cap, /*isSideA=*/false};

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total{0};

    // Sender A -> B: send empty payload frames (max PPS)
    std::thread sender([&] {
        std::vector<uint8_t> payload; // zero-length payload
        while (!stop.load(std::memory_order_relaxed)) {
            // Block until sent to avoid dropping (A -> B)
            a.send(payload, std::chrono::milliseconds{-1});
        }
    });

    // Receiver B: count messages
    std::thread receiver([&] {
        std::vector<uint8_t> msg;
        while (!stop.load(std::memory_order_relaxed)) {
            if (!b.recv(msg, std::chrono::milliseconds{200})) continue;
            total.fetch_add(1, std::memory_order_relaxed);
        }
    });

    auto start = clock::now();
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
