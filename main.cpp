// Simple smoke test: create both sides, send a message from A to B.
#include "capnproto_shm_transport.hpp"
#include <iostream>
#include <thread>
#include <cstring>
#include <vector>

using capnproto_shm_transport::ShmDuplexTransport;

int main() {
    std::cout << "capnproto_shm_transport version: "
                        << capnproto_shm_transport::version() << std::endl;

    const std::string name = "capnp-demo";
    const std::size_t cap = 64 * 1024;

    ShmDuplexTransport::remove(name); // clean start

    ShmDuplexTransport a{name, cap, /*isSideA=*/true, /*openOrCreate=*/true, /*truncateOnCreate=*/true};
    std::thread t([&] {
        ShmDuplexTransport b{name, cap, /*isSideA=*/false};
        std::vector<uint8_t> msg;
        if (b.recv(msg, std::chrono::milliseconds{1000})) {
            std::string s(msg.begin(), msg.end());
            std::cout << "B received: " << s << std::endl;
        } else {
            std::cout << "B recv timeout" << std::endl;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const char* text = "hello over shm";
        std::vector<uint8_t> payload(std::strlen(text));
        std::memcpy(payload.data(), text, payload.size());
    if (!a.send(payload, std::chrono::milliseconds{1000})) {
        std::cerr << "A send failed" << std::endl;
    }

    t.join();
    ShmDuplexTransport::remove(name);
    return 0;
}
