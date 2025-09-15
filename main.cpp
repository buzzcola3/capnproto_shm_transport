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
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <time.h>

using clock_type = std::chrono::steady_clock;

static constexpr const char* kTransportName = "capnproto_fixedslot_demo";
static constexpr std::chrono::milliseconds kOpenWait{500};
static constexpr std::chrono::milliseconds kMsgPeriod{30}; // 1 message / 30ms

// Helper
struct CpuUsageSnapshot {
    double user_s{0};
    double sys_s{0};
    double proc_cpu_s{0};
    static double toSec(const timeval& tv){ return tv.tv_sec + tv.tv_usec/1e6; }
    static CpuUsageSnapshot now() {
        CpuUsageSnapshot s;
        rusage ru{};
        getrusage(RUSAGE_SELF, &ru);
        s.user_s = toSec(ru.ru_utime);
        s.sys_s  = toSec(ru.ru_stime);
        timespec ts{};
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        s.proc_cpu_s = ts.tv_sec + ts.tv_nsec/1e9;
        return s;
    }
};

struct CpuUsageReport {
    CpuUsageSnapshot start;
    void begin() { start = CpuUsageSnapshot::now(); }
    void end(double wallSeconds, const char* label) {
        auto end = CpuUsageSnapshot::now();
        double dUser = end.user_s - start.user_s;
        double dSys  = end.sys_s  - start.sys_s;
        double dCpu  = dUser + dSys;
        double pct   = wallSeconds > 0 ? (dCpu / wallSeconds) * 100.0 : 0;
        std::cerr << "[cpu] " << label
                  << " user=" << dUser
                  << " sys="  << dSys
                  << " cpu="  << dCpu
                  << " wall=" << wallSeconds
                  << " pct="  << std::fixed << std::setprecision(2) << pct << "% (rounded="
                  << static_cast<long long>(pct + 0.5) << "%)\n";
    }
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " --a   <seconds> <slotSize> <slotCount>        (burst)\n"
                  << "  " << argv[0] << " --b   <seconds>                                (burst)\n"
                  << "  " << argv[0] << " --a30 <seconds> <slotSize> <slotCount>         (30ms rate)\n"
                  << "  " << argv[0] << " --b30 <seconds>                                 (30ms rate)\n";
        return 2;
    }
    std::string mode = argv[1];
    bool isA    = mode == "--a";
    bool isB    = mode == "--b";
    bool isA30  = mode == "--a30";
    bool isB30  = mode == "--b30";
    if (!isA && !isB && !isA30 && !isB30) return 2;

    try {
        CpuUsageReport cpu;
        if (isA || isA30) {
            if ((isA  && argc != 5) ||
                (isA30 && argc != 5)) return 2;
            long long seconds = std::stoll(argv[2]);
            uint64_t slotSize  = std::stoull(argv[3]);
            uint64_t slotCount = std::stoull(argv[4]);
            if (seconds <= 0 || slotSize == 0 || (slotCount & (slotCount - 1))) {
                std::cerr << "Invalid args\n"; return 2;
            }
            capnproto_shm_transport::ShmFixedSlotDuplexTransport::remove(kTransportName);

            std::atomic<uint64_t> received{0};
            capnproto_shm_transport::ShmFixedSlotDuplexTransport transport(
                kTransportName, slotSize, slotCount, true,
                [&received](const uint8_t* d, uint64_t l) {
                    (void)d; (void)l;
                    received.fetch_add(1, std::memory_order_relaxed);
                });

            std::vector<uint8_t> buf(slotSize);
            uint64_t seq = 0, sent = 0;
            auto start = clock_type::now();
            auto end   = start + std::chrono::seconds(seconds);

            cpu.begin();
            if (isA) {
                // Burst mode
                while (clock_type::now() < end) {
                    std::memcpy(buf.data(), &seq, sizeof(seq));
                    if (!transport.sendSlot(buf.data(), buf.size(), std::chrono::milliseconds{-1})) break;
                    ++seq; ++sent;
                }
            } else {
                // Rate-limited: 1 message every 30ms (best-effort)
                auto nextSend = start;
                while (clock_type::now() < end) {
                    auto now = clock_type::now();
                    if (now >= nextSend) {
                        std::memcpy(buf.data(), &seq, sizeof(seq));
                        if (!transport.sendSlot(buf.data(), buf.size(), std::chrono::milliseconds{-1})) break;
                        ++seq; ++sent;
                        nextSend += kMsgPeriod;
                        // Catch up if behind (skip missed periods)
                        while (nextSend + kMsgPeriod < now) nextSend += kMsgPeriod;
                    }
                    std::this_thread::sleep_until(std::min(nextSend, end));
                }
            }

            double elapsed = std::chrono::duration<double>(clock_type::now() - start).count();
            std::cout << "role=" << (isA ? 'A' : 'A')
                      << (isA30 ? " (30ms)" : " (burst)")
                      << " sent=" << sent
                      << " recv=" << received.load()
                      << " send_pps=" << (long long)(sent / (elapsed > 0 ? elapsed : 1))
                      << " recv_pps=" << (long long)(received.load() / (elapsed > 0 ? elapsed : 1))
                      << "\n";
            cpu.end(elapsed, isA30 ? "A30" : "A");
            capnproto_shm_transport::ShmFixedSlotDuplexTransport::remove(kTransportName);
        } else { // isB || isB30
            if ((isB  && argc != 3) ||
                (isB30 && argc != 3)) return 2;
            long long seconds = std::stoll(argv[2]);
            if (seconds <= 0) return 2;

            std::atomic<uint64_t> received{0};
            auto transport = capnproto_shm_transport::ShmFixedSlotDuplexTransport::open(
                kTransportName, kOpenWait,
                [&received](const uint8_t* d, uint64_t l) {
                    (void)d; (void)l;
                    received.fetch_add(1, std::memory_order_relaxed);
                });

            uint64_t slotSize = transport.slotSize();
            std::vector<uint8_t> buf(slotSize);
            uint64_t seq = 0, sent = 0;

            cpu.begin();
            auto start = clock_type::now();
            auto end   = start + std::chrono::seconds(seconds);

            if (isB) {
                // Burst
                while (clock_type::now() < end) {
                    std::memcpy(buf.data(), &seq, sizeof(seq));
                    if (!transport.sendSlot(buf.data(), buf.size(), std::chrono::milliseconds{-1})) break;
                    ++seq; ++sent;
                }
            } else {
                // 30ms rate-limited
                auto nextSend = start;
                while (clock_type::now() < end) {
                    auto now = clock_type::now();
                    if (now >= nextSend) {
                        std::memcpy(buf.data(), &seq, sizeof(seq));
                        if (!transport.sendSlot(buf.data(), buf.size(), std::chrono::milliseconds{-1})) break;
                        ++seq; ++sent;
                        nextSend += kMsgPeriod;
                        while (nextSend + kMsgPeriod < now) nextSend += kMsgPeriod;
                    }
                    std::this_thread::sleep_until(std::min(nextSend, end));
                }
            }

            double elapsed = std::chrono::duration<double>(clock_type::now() - start).count();
            std::cout << "role=" << (isB ? 'B' : 'B')
                      << (isB30 ? " (30ms)" : " (burst)")
                      << " sent=" << sent
                      << " recv=" << received.load()
                      << " send_pps=" << (long long)(sent / (elapsed > 0 ? elapsed : 1))
                      << " recv_pps=" << (long long)(received.load() / (elapsed > 0 ? elapsed : 1))
                      << "\n";
            cpu.end(elapsed, isB30 ? "B30" : "B");
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "error: unknown\n";
        return 1;
    }
    return 0;
}
