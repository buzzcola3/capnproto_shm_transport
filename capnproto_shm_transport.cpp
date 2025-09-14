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

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

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

struct alignas(64) SlotHeader {
    std::size_t slotSize{0};
    std::size_t slotCount{0};
    std::size_t slotMask{0};
    std::atomic<std::size_t> head{0};
    char pad0[64 - sizeof(std::atomic<std::size_t>) % 64];
    std::atomic<std::size_t> tail{0};
    char pad1[64 - sizeof(std::atomic<std::size_t>) % 64];
    std::atomic<bool> shutdown{false};
    std::atomic<bool> ready{false}; // published when creator finishes init
};

struct SlotLayout { SlotHeader* hdr{nullptr}; uint8_t* buf{nullptr}; };

inline std::size_t slots_used(const SlotHeader* h) {
    auto head = h->head.load(std::memory_order_relaxed);
    auto tail = h->tail.load(std::memory_order_relaxed);
    return (tail - head) & h->slotMask;
}

SlotLayout create_region(bip::managed_shared_memory& shm,
                         std::size_t slotSize,
                         std::size_t slotCount) {
    if ((slotCount & (slotCount - 1)) != 0)
        throw std::runtime_error("slotCount must be power of two");
    auto* hdr = shm.construct<SlotHeader>("hdr")();
    hdr->slotSize  = slotSize;
    hdr->slotCount = slotCount;
    hdr->slotMask  = slotCount - 1;
    hdr->head.store(0, std::memory_order_relaxed);
    hdr->tail.store(0, std::memory_order_relaxed);
    hdr->shutdown.store(false, std::memory_order_relaxed);
    hdr->ready.store(false, std::memory_order_relaxed);
    auto* buf = shm.construct<uint8_t>("buf")[slotSize * slotCount]();
    return {hdr, buf};
}

SlotLayout open_region(bip::managed_shared_memory& shm) {
    auto hfound = shm.find<SlotHeader>("hdr");
    auto dfound = shm.find<uint8_t>("buf");
    if (!hfound.first || !dfound.first)
        throw std::runtime_error("region not initialized yet");
    return {hfound.first, dfound.first};
}

bool slot_write(SlotLayout lay, const uint8_t* src,
                std::chrono::milliseconds timeout) {
    auto* h = lay.hdr;
    auto deadline = (timeout < std::chrono::milliseconds{0})
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto head = h->head.load(std::memory_order_acquire);
        auto tail = h->tail.load(std::memory_order_relaxed);
        if (((tail - head) & h->slotMask) != h->slotMask) {
            std::size_t idx = tail & h->slotMask;
            std::memcpy(lay.buf + idx * h->slotSize, src, h->slotSize);
            h->tail.store(tail + 1, std::memory_order_release);
            return true;
        }
        if (h->shutdown.load(std::memory_order_relaxed)) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        cpu_relax();
    }
}

bool slot_read(SlotLayout lay, uint8_t* dst,
               std::chrono::milliseconds timeout) {
    auto* h = lay.hdr;
    auto deadline = (timeout < std::chrono::milliseconds{0})
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto head = h->head.load(std::memory_order_relaxed);
        auto tail = h->tail.load(std::memory_order_acquire);
        if (((tail - head) & h->slotMask) != 0) {
            std::size_t idx = head & h->slotMask;
            std::memcpy(dst, lay.buf + idx * h->slotSize, h->slotSize);
            h->head.store(head + 1, std::memory_order_release);
            return true;
        }
        if (h->shutdown.load(std::memory_order_relaxed)) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        cpu_relax();
    }
}
} // namespace

struct ShmFixedSlotDuplexTransport::Impl {
    std::string name;
    bool creator{false};
    std::size_t slotSize{0};
    std::size_t slotCount{0};
    std::unique_ptr<bip::managed_shared_memory> seg_tx;
    std::unique_ptr<bip::managed_shared_memory> seg_rx;
    SlotLayout tx{};
    SlotLayout rx{};

    // Creator
    Impl(const std::string& base,
         std::size_t sSize,
         std::size_t sCount,
         bool truncate) : name(base), creator(true),
                          slotSize(sSize), slotCount(sCount) {
        std::string a2b = base + ".fs.a2b";
        std::string b2a = base + ".fs.b2a";
        if (truncate) {
            bip::shared_memory_object::remove(a2b.c_str());
            bip::shared_memory_object::remove(b2a.c_str());
        }
        auto alloc_bytes = [&](std::size_t sz) {
            // header + payload + slack
            return sizeof(SlotHeader) + sSize * sCount + 4096;
        };
        seg_tx = std::make_unique<bip::managed_shared_memory>(bip::create_only, a2b.c_str(),
                                                              alloc_bytes(sSize * sCount));
        seg_rx = std::make_unique<bip::managed_shared_memory>(bip::create_only, b2a.c_str(),
                                                              alloc_bytes(sSize * sCount));
        tx = create_region(*seg_tx, slotSize, slotCount);
        rx = create_region(*seg_rx, slotSize, slotCount);
        // Publish headers ready
        tx.hdr->ready.store(true, std::memory_order_release);
        rx.hdr->ready.store(true, std::memory_order_release);
    }

    // Opener (Side B)
    Impl(const std::string& base,
         std::chrono::milliseconds wait)
         : name(base), creator(false) {
        std::string a2b = base + ".fs.a2b";
        std::string b2a = base + ".fs.b2a";
        auto deadline = std::chrono::steady_clock::now() + wait;
        for (;;) {
            try {
                seg_rx = std::make_unique<bip::managed_shared_memory>(bip::open_only, a2b.c_str());
                seg_tx = std::make_unique<bip::managed_shared_memory>(bip::open_only, b2a.c_str());
                rx = open_region(*seg_rx);
                tx = open_region(*seg_tx);
                // Wait until creator published sizes
                if (rx.hdr->ready.load(std::memory_order_acquire) &&
                    tx.hdr->ready.load(std::memory_order_acquire)) {
                    slotSize  = rx.hdr->slotSize;
                    slotCount = rx.hdr->slotCount;
                    return;
                }
            } catch (...) {
                // ignore until timeout
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("timeout opening shared memory transport");
            cpu_relax();
        }
    }

    ~Impl() {
        for (auto lay : {tx, rx}) {
            if (lay.hdr) lay.hdr->shutdown.store(true, std::memory_order_relaxed);
        }
    }
};

// Creator constructor
ShmFixedSlotDuplexTransport::ShmFixedSlotDuplexTransport(const std::string& name,
                                                         std::size_t slotSize,
                                                         std::size_t slotCount,
                                                         bool truncateOnCreate)
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

bool ShmFixedSlotDuplexTransport::sendSlot(const uint8_t* data,
                                           std::size_t len,
                                           std::chrono::milliseconds timeout) {
    if (!p_ || !data) return false;
    if (len != p_->slotSize) return false;
    return slot_write(p_->tx, data, timeout);
}

bool ShmFixedSlotDuplexTransport::recvSlot(std::vector<uint8_t>& out,
                                           std::chrono::milliseconds timeout) {
    if (!p_) return false;
    out.resize(p_->slotSize);
    return slot_read(p_->rx, out.data(), timeout);
}

bool ShmFixedSlotDuplexTransport::getStats(SlotTransportStats& out) {
    if (!p_) return false;
    auto calc = [](SlotLayout lay) {
        SlotRingStats s{};
        if (!lay.hdr) return s;
        s.slotSize  = lay.hdr->slotSize;
        s.slotCount = lay.hdr->slotCount;
        s.usedSlots = slots_used(lay.hdr);
        s.shutdown  = lay.hdr->shutdown.load(std::memory_order_relaxed);
        return s;
    };
    out.tx = calc(p_->tx);
    out.rx = calc(p_->rx);
    return true;
}

std::size_t ShmFixedSlotDuplexTransport::slotSize() const noexcept {
    return p_ ? p_->slotSize : 0;
}
std::size_t ShmFixedSlotDuplexTransport::slotCount() const noexcept {
    return p_ ? p_->slotCount : 0;
}
bool ShmFixedSlotDuplexTransport::isCreator() const noexcept {
    return p_ ? p_->creator : false;
}

void ShmFixedSlotDuplexTransport::remove(const std::string& name) {
    bip::shared_memory_object::remove((name + ".fs.a2b").c_str());
    bip::shared_memory_object::remove((name + ".fs.b2a").c_str());
}

} // namespace capnproto_shm_transport

