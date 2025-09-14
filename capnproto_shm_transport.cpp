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

#include "capnproto_shm_transport.hpp"

#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>  // <— added for debug output

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#define CAPNPROTO_SHM_DEBUG

namespace bip = boost::interprocess;

namespace capnproto_shm_transport {

std::string version() { return "0.1.0"; }

// ===================== Fixed-slot transport (Creator/Open) =====================
namespace {
inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// Control / ring header (fixed POD, validated by magic+abiVersion)
struct alignas(64) SlotHeader {
    uint32_t magic{0};        // CAPNPROTO_SHM_TRANSPORT_MAGIC
    uint32_t abiVersion{0};   // CAPNPROTO_SHM_TRANSPORT_ABI_VERSION
    uint64_t slotSize{0};
    uint64_t slotCount{0};
    uint64_t slotMask{0};
    uint32_t shutdown{0};
    uint32_t ready{0};
    uint8_t  pad0[64 - (4+4+8+8+8+4+4)]; // 64 - 40 = 24
    std::atomic<uint64_t> head{0};
    uint8_t  pad1[64 - sizeof(std::atomic<uint64_t>)];
    std::atomic<uint64_t> tail{0};
    uint8_t  pad2[64 - sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(SlotHeader) == 192, "SlotHeader size unexpected");

struct SlotLayout {
    SlotHeader* hdr{nullptr};
    uint8_t*    buf{nullptr};
};

inline uint64_t slots_used(const SlotHeader* h) {
    auto head = h->head.load(std::memory_order_relaxed);
    auto tail = h->tail.load(std::memory_order_relaxed);
    return (tail - head) & h->slotMask;
}

inline void validate_header(const SlotHeader* h) {
    if (!h ||
        h->magic != CAPNPROTO_SHM_TRANSPORT_MAGIC ||
        h->abiVersion != CAPNPROTO_SHM_TRANSPORT_ABI_VERSION ||
        h->slotCount == 0 ||
        ((h->slotCount & (h->slotCount - 1)) != 0) ||
        h->slotMask != (h->slotCount - 1)) {
        throw std::runtime_error("shared memory header invalid/mismatch");
    }
}

SlotLayout create_region(bip::managed_shared_memory& shm,
                         uint64_t slotSize,
                         uint64_t slotCount) {
    if ((slotCount & (slotCount - 1)) != 0)
        throw std::runtime_error("slotCount must be power of two");
    auto* hdr = shm.construct<SlotHeader>("hdr")();
    hdr->magic      = CAPNPROTO_SHM_TRANSPORT_MAGIC;
    hdr->abiVersion = CAPNPROTO_SHM_TRANSPORT_ABI_VERSION;
    hdr->slotSize   = slotSize;
    hdr->slotCount  = slotCount;
    hdr->slotMask   = slotCount - 1;
    hdr->shutdown   = 0;
    hdr->ready      = 0;
    auto* buf = shm.construct<uint8_t>("buf")[slotSize * slotCount]();
    return {hdr, buf};
}

SlotLayout open_region(bip::managed_shared_memory& shm) {
    auto hfound = shm.find<SlotHeader>("hdr");
    auto dfound = shm.find<uint8_t>("buf");
    if (!hfound.first || !dfound.first)
        throw std::runtime_error("region not initialized yet");
    validate_header(hfound.first);
    return {hfound.first, dfound.first};
}

uint32_t slot_write(SlotLayout lay, const uint8_t* src,
                    std::chrono::milliseconds timeout) {
    auto* h = lay.hdr;
    auto deadline = (timeout < std::chrono::milliseconds{0})
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto head = h->head.load(std::memory_order_acquire);
        auto tail = h->tail.load(std::memory_order_relaxed);
        if (((tail - head) & h->slotMask) != h->slotMask) {
            uint64_t idx = tail & h->slotMask;
            std::memcpy(lay.buf + idx * h->slotSize, src, h->slotSize);
            h->tail.store(tail + 1, std::memory_order_release);
            return 1;
        }
        if (h->shutdown) return 0;
        if (std::chrono::steady_clock::now() >= deadline) return 0;
        cpu_relax();
    }
}

uint32_t slot_read(SlotLayout lay, uint8_t* dst,
                   std::chrono::milliseconds timeout) {
    auto* h = lay.hdr;
    auto deadline = (timeout < std::chrono::milliseconds{0})
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto head = h->head.load(std::memory_order_relaxed);
        auto tail = h->tail.load(std::memory_order_acquire);
        if (((tail - head) & h->slotMask) != 0) {
            uint64_t idx = head & h->slotMask;
            std::memcpy(dst, lay.buf + idx * h->slotSize, h->slotSize);
            h->head.store(head + 1, std::memory_order_release);
            return 1;
        }
        if (h->shutdown) return 0;
        if (std::chrono::steady_clock::now() >= deadline) return 0;
        cpu_relax();
    }
}
} // namespace

struct ShmFixedSlotDuplexTransport::Impl {
    std::string name;
    uint32_t creator{0};
    uint64_t slotSize{0};
    uint64_t slotCount{0};
    std::unique_ptr<bip::managed_shared_memory> seg_tx;
    std::unique_ptr<bip::managed_shared_memory> seg_rx;
    SlotLayout tx{};
    SlotLayout rx{};

    Impl(const std::string& base,
         uint64_t sSize,
         uint64_t sCount,
         uint32_t truncate) : name(base), creator(1),
                              slotSize(sSize), slotCount(sCount) {
        std::string a2b = base + ".fs.a2b";
        std::string b2a = base + ".fs.b2a";
        if (truncate) {
            bip::shared_memory_object::remove(a2b.c_str());
            bip::shared_memory_object::remove(b2a.c_str());
        }
        auto alloc_bytes = [&](uint64_t) {
            return static_cast<uint64_t>(sizeof(SlotHeader)) + sSize * sCount + 4096;
        };
        seg_tx = std::make_unique<bip::managed_shared_memory>(bip::create_only, a2b.c_str(),
                                                              alloc_bytes(sSize * sCount));
        seg_rx = std::make_unique<bip::managed_shared_memory>(bip::create_only, b2a.c_str(),
                                                              alloc_bytes(sSize * sCount));
        tx = create_region(*seg_tx, slotSize, slotCount);
        rx = create_region(*seg_rx, slotSize, slotCount);
        tx.hdr->ready = 1;
        rx.hdr->ready = 1;
    }

    Impl(const std::string& base,
         std::chrono::milliseconds wait)
         : name(base), creator(0) {
        std::string a2b = base + ".fs.a2b";
        std::string b2a = base + ".fs.b2a";
        auto deadline = std::chrono::steady_clock::now() + wait;
        auto start    = std::chrono::steady_clock::now();
        uint64_t attempts = 0;
#ifdef CAPNPROTO_SHM_DEBUG
        std::cerr << "[capnproto_shm_transport] opener waiting for segments: "
                  << a2b << " & " << b2a << " (timeout "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(wait).count()
                  << " ms)" << std::endl;
#endif
        for (;;) {
            ++attempts;
            try {
                seg_rx = std::make_unique<bip::managed_shared_memory>(bip::open_only, a2b.c_str());
                seg_tx = std::make_unique<bip::managed_shared_memory>(bip::open_only, b2a.c_str());
                rx = open_region(*seg_rx);   // may throw if header invalid / not yet constructed
                tx = open_region(*seg_tx);
                // Validate creator published readiness
                if (rx.hdr->ready && tx.hdr->ready) {
                    slotSize  = rx.hdr->slotSize;
                    slotCount = rx.hdr->slotCount;
#ifdef CAPNPROTO_SHM_DEBUG
                    std::cerr << "[capnproto_shm_transport] opener connected after "
                              << attempts << " attempts ("
                              << std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start).count()
                              << " ms). slotSize=" << slotSize
                              << " slotCount=" << slotCount << std::endl;
#endif
                    return;
                }
#ifdef CAPNPROTO_SHM_DEBUG
                std::cerr << "[capnproto_shm_transport] headers present but not ready yet "
                          << "(attempt " << attempts << ")" << std::endl;
#endif
            } catch (const std::exception& ex) {
#ifdef CAPNPROTO_SHM_DEBUG
                // Only log periodically to avoid flooding
                if ((attempts & 0xFF) == 0) {
                    std::cerr << "[capnproto_shm_transport] still waiting (attempt "
                              << attempts << "): " << ex.what() << std::endl;
                }
#endif
            } catch (...) {
#ifdef CAPNPROTO_SHM_DEBUG
                if ((attempts & 0xFF) == 0) {
                    std::cerr << "[capnproto_shm_transport] still waiting (attempt "
                              << attempts << "): unknown exception" << std::endl;
                }
#endif
            }
            if (std::chrono::steady_clock::now() >= deadline) {
#ifdef CAPNPROTO_SHM_DEBUG
                std::cerr << "[capnproto_shm_transport] TIMEOUT opening shared memory transport after "
                          << attempts << " attempts. Last state:"
                          << " rx_seg=" << a2b
                          << " tx_seg=" << b2a << std::endl;
#endif
                throw std::runtime_error("timeout opening shared memory transport");
            }
            cpu_relax();
        }
    }

    ~Impl() {
        if (tx.hdr) tx.hdr->shutdown = 1;
        if (rx.hdr) rx.hdr->shutdown = 1;
    }
};

// Creator
ShmFixedSlotDuplexTransport::ShmFixedSlotDuplexTransport(const std::string& name,
                                                         uint64_t slotSize,
                                                         uint64_t slotCount,
                                                         uint32_t truncateOnCreate)
    : p_(new Impl(name, slotSize, slotCount, truncateOnCreate)) {}

ShmFixedSlotDuplexTransport::ShmFixedSlotDuplexTransport(Impl* impl) : p_(impl) {}

ShmFixedSlotDuplexTransport ShmFixedSlotDuplexTransport::open(const std::string& name,
                                                              std::chrono::milliseconds wait) {
    return ShmFixedSlotDuplexTransport(new Impl(name, wait));
}

ShmFixedSlotDuplexTransport::~ShmFixedSlotDuplexTransport() { delete p_; }

ShmFixedSlotDuplexTransport::ShmFixedSlotDuplexTransport(ShmFixedSlotDuplexTransport&& o) noexcept : p_(o.p_) {
    o.p_ = nullptr;
}
ShmFixedSlotDuplexTransport&
ShmFixedSlotDuplexTransport::operator=(ShmFixedSlotDuplexTransport&& o) noexcept {
    if (this != &o) { delete p_; p_ = o.p_; o.p_ = nullptr; }
    return *this;
}

uint32_t ShmFixedSlotDuplexTransport::sendSlot(const uint8_t* data,
                                               uint64_t len,
                                               std::chrono::milliseconds timeout) {
    if (!p_ || !data) return 0;
    if (len != p_->slotSize) return 0;
    return slot_write(p_->tx, data, timeout);
}

uint32_t ShmFixedSlotDuplexTransport::recvSlot(std::vector<uint8_t>& out,
                                               std::chrono::milliseconds timeout) {
    if (!p_) return 0;
    out.resize(p_->slotSize);
    return slot_read(p_->rx, out.data(), timeout);
}

uint32_t ShmFixedSlotDuplexTransport::getStats(SlotTransportStats& out) {
    if (!p_) return 0;
    auto calc = [](SlotLayout lay) {
        SlotRingStats s{};
        if (!lay.hdr) return s;
        s.slotSize  = lay.hdr->slotSize;
        s.slotCount = lay.hdr->slotCount;
        s.usedSlots = slots_used(lay.hdr);
        s.shutdown  = lay.hdr->shutdown;
        return s;
    };
    out.tx = calc(p_->tx);
    out.rx = calc(p_->rx);
    return 1;
}

uint64_t ShmFixedSlotDuplexTransport::slotSize() const noexcept {
    return p_ ? p_->slotSize : 0;
}
uint64_t ShmFixedSlotDuplexTransport::slotCount() const noexcept {
    return p_ ? p_->slotCount : 0;
}
uint32_t ShmFixedSlotDuplexTransport::isCreator() const noexcept {
    return p_ ? p_->creator : 0;
}

void ShmFixedSlotDuplexTransport::remove(const std::string& name) {
    bip::shared_memory_object::remove((name + ".fs.a2b").c_str());
    bip::shared_memory_object::remove((name + ".fs.b2a").c_str());
}

} // namespace capnproto_shm_transport

