/*
*  This file is part of OpenAutoCore project.
*  Copyright (C) 2025 buzzcola3 (Samuel Betak)
*
*  OpenAutoCore is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.

*  OpenAutoCore is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with OpenAutoCore. If not, see <http://www.gnu.org/licenses/>.
*/

// Measures:
// - messages per second (A -> B)
#include "capnproto_shm_transport.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using clock_type = std::chrono::steady_clock;

static void usage(const char* prog, const char* msg = nullptr) {
    if (msg) std::cerr << msg << "\n";
    std::cerr
        << "Usage:\n"
        << "  " << prog << " --create <name> <seconds> <slotSize> <slotCount>\n"
        << "  " << prog << " --open   <name> <seconds>\n"
        << "\nDescription:\n"
        << "  --create : Side A – creates the shared memory (specifies slot size/count) and sends.\n"
        << "  --open   : Side B – opens existing transport (reads size/count from header) and receives.\n"
        << "\nOutput:\n"
        << "  Prints a single line at end: pps=<messages_per_second>\n"
        << std::endl;
}

int main(int argc, char** argv) {
    const char* prog = (argc > 0 && argv[0]) ? argv[0] : "prog";
    if (argc < 2) {
        usage(prog, "Missing mode");
        return 2;
    }

    std::string mode = argv[1];
    bool isCreate = (mode == "--create");
    bool isOpen   = (mode == "--open");

    if (!isCreate && !isOpen) {
        usage(prog, "First argument must be --create or --open");
        return 2;
    }

    try {
        if (isCreate) {
            if (argc != 6) {
                usage(prog, "Invalid args for --create");
                return 2;
            }
            std::string name = argv[2];
            long long seconds = std::stoll(argv[3]);
            if (seconds <= 0) {
                usage(prog, "seconds must be > 0");
                return 2;
            }
            std::size_t slotSize  = static_cast<std::size_t>(std::stoull(argv[4]));
            std::size_t slotCount = static_cast<std::size_t>(std::stoull(argv[5]));
            if (slotSize == 0 || slotCount < 2) {
                usage(prog, "slotSize must be >0 and slotCount >1");
                return 2;
            }

            // Ensure old segments gone
            capnproto_shm_transport::ShmFixedSlotDuplexTransport::remove(name);

            capnproto_shm_transport::ShmFixedSlotDuplexTransport tx{name, slotSize, slotCount, /*truncateOnCreate=*/true};

            std::vector<uint8_t> payload(slotSize, 0);
            // Embed a monotonically increasing 64-bit sequence in first 8 bytes (optional)
            uint64_t seq = 0;
            auto start = clock_type::now();
            auto endTime = start + std::chrono::seconds(seconds);
            uint64_t sent = 0;

            while (clock_type::now() < endTime) {
                std::memcpy(payload.data(), &seq, sizeof(seq));
                if (!tx.sendSlot(payload.data(), payload.size(), std::chrono::milliseconds{-1})) {
                    break; // shutdown or unexpected failure
                }
                ++seq;
                ++sent;
            }

            double elapsed = std::chrono::duration<double>(clock_type::now() - start).count();
            double pps = elapsed > 0.0 ? static_cast<double>(sent) / elapsed : 0.0;
            std::cout << "pps=" << static_cast<long long>(pps) << std::endl;

            // Cleanup (only creator removes)
            capnproto_shm_transport::ShmFixedSlotDuplexTransport::remove(name);
        } else if (isOpen) {
            if (argc != 4) {
                usage(prog, "Invalid args for --open");
                return 2;
            }
            std::string name = argv[2];
            long long seconds = std::stoll(argv[3]);
            if (seconds <= 0) {
                usage(prog, "seconds must be > 0");
                return 2;
            }

            auto rxTransport = capnproto_shm_transport::ShmFixedSlotDuplexTransport::open(name);

            std::vector<uint8_t> slot;
            uint64_t received = 0;
            auto start = clock_type::now();
            auto endTime = start + std::chrono::seconds(seconds);

            while (clock_type::now() < endTime) {
                if (rxTransport.recvSlot(slot, std::chrono::milliseconds{200})) {
                    ++received;
                }
            }

            double elapsed = std::chrono::duration<double>(clock_type::now() - start).count();
            double pps = elapsed > 0.0 ? static_cast<double>(received) / elapsed : 0.0;
            std::cout << "pps=" << static_cast<long long>(pps) << std::endl;
            // Opener does not remove segments.
        }
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "error: unknown exception\n";
        return 1;
    }
    return 0;
}
