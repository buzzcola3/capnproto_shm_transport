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
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  if defined(_MSC_VER)
    _mm_pause();
#  else
    _mm_pause();
#  endif
#elif defined(__aarch64__)
#  if defined(_MSC_VER)
    __yield();
#  else
    asm volatile("yield" ::: "memory");
#  endif
#else
    // Fallback: cooperative yield
    std::this_thread::yield();
#endif
}

struct alignas(64) SlotHeader {
    uint64_t slotSize{0};
    uint64_t slotCount{0};
    uint64_t slotMask{0};
    uint32_t shutdown{0};
    uint32_t ready{0};
    std::atomic<uint32_t> pollIntervalAUs{1000};
    std::atomic<uint32_t> pollIntervalBUs{1000};
    uint8_t  _pad0[64 - (8+8+8+4+4+4+4)];
    std::atomic<uint64_t> head{0};
    uint8_t  _pad1[64 - sizeof(std::atomic<uint64_t>)];
    std::atomic<uint64_t> tail{0};
    uint8_t  _pad2[64 - sizeof(std::atomic<uint64_t>)];
};
// Adjust expected size (3 cache lines = 192 bytes)
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

SlotLayout create_region(bip::managed_shared_memory& shm,
                         uint64_t slotSize,
                         uint64_t slotCount) {
    if ((slotCount & (slotCount - 1)) != 0)
        throw std::runtime_error("slotCount must be power of two");
    auto* hdr = shm.construct<SlotHeader>("hdr")();
    hdr->slotSize  = slotSize;
    hdr->slotCount = slotCount;
    hdr->slotMask  = slotCount - 1;
    hdr->shutdown  = 0;
    hdr->ready     = 0;
    hdr->pollIntervalAUs.store(1000, std::memory_order_relaxed);
    hdr->pollIntervalBUs.store(1000, std::memory_order_relaxed);
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
    uint32_t creator{0}; // side A if 1, side B if 0
    uint64_t slotSize{0};
    uint64_t slotCount{0};
    std::unique_ptr<bip::managed_shared_memory> seg_tx;
    std::unique_ptr<bip::managed_shared_memory> seg_rx;
    SlotLayout tx{};
    SlotLayout rx{};
    std::function<void(const uint8_t*, uint64_t)> callback;
    std::atomic<bool> running{false};
    std::thread worker;

    Impl(const std::string& base,
         uint64_t sSize,
         uint64_t sCount,
         uint32_t truncate,
         std::function<void(const uint8_t*, uint64_t)> cb,
         std::chrono::microseconds initialPoll)
        : name(base), creator(1), slotSize(sSize), slotCount(sCount),
          callback(std::move(cb)) {

        std::string a2b = base + ".fs.a2b";
        std::string b2a = base + ".fs.b2a";
        if (truncate) {
            bip::shared_memory_object::remove(a2b.c_str());
            bip::shared_memory_object::remove(b2a.c_str());
        }
        auto alloc_bytes = [&](uint64_t) {
            return static_cast<uint64_t>(sizeof(SlotHeader)) + sSize * sCount + 4096;
        };
        seg_tx = std::make_unique<bip::managed_shared_memory>(
            bip::create_only, a2b.c_str(), alloc_bytes(sSize * sCount));
        seg_rx = std::make_unique<bip::managed_shared_memory>(
            bip::create_only, b2a.c_str(), alloc_bytes(sSize * sCount));
        tx = create_region(*seg_tx, slotSize, slotCount);
        rx = create_region(*seg_rx, slotSize, slotCount);
        // Set A side poll interval in both headers (duplication)
        tx.hdr->pollIntervalAUs.store(static_cast<uint32_t>(initialPoll.count()), std::memory_order_relaxed);
        rx.hdr->pollIntervalAUs.store(static_cast<uint32_t>(initialPoll.count()), std::memory_order_relaxed);
        tx.hdr->ready = 1;
        rx.hdr->ready = 1;
        if (callback) startThread();
    }

    Impl(const std::string& base,
         std::chrono::milliseconds wait,
         std::function<void(const uint8_t*, uint64_t)> cb,
         std::chrono::microseconds initialPoll)
        : name(base), creator(0), callback(std::move(cb)) {

        std::string a2b = base + ".fs.a2b";
        std::string b2a = base + ".fs.b2a";
        auto deadline = std::chrono::steady_clock::now() + wait;
        for (;;) {
            try {
                seg_rx = std::make_unique<bip::managed_shared_memory>(bip::open_only, a2b.c_str());
                seg_tx = std::make_unique<bip::managed_shared_memory>(bip::open_only, b2a.c_str());
                rx = open_region(*seg_rx);
                tx = open_region(*seg_tx);
                if (rx.hdr->ready && tx.hdr->ready) {
                    slotSize  = rx.hdr->slotSize;
                    slotCount = rx.hdr->slotCount;
                    break;
                }
            } catch (...) {}
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("timeout opening shared memory transport");
            cpu_relax();
        }
        // Set B side poll interval (both headers for symmetry)
        rx.hdr->pollIntervalBUs.store(static_cast<uint32_t>(initialPoll.count()), std::memory_order_relaxed);
        tx.hdr->pollIntervalBUs.store(static_cast<uint32_t>(initialPoll.count()), std::memory_order_relaxed);
        if (callback) startThread();
    }

    bool tryConsumeOne() {
        if (!rx.hdr) return false;
        auto* h = rx.hdr;
        auto head = h->head.load(std::memory_order_relaxed);
        auto tail = h->tail.load(std::memory_order_acquire);
        if (((tail - head) & h->slotMask) == 0)
            return false;
        uint64_t idx = head & h->slotMask;
        uint8_t* ptr = rx.buf + idx * h->slotSize;
        h->head.store(head + 1, std::memory_order_release);
        if (callback) callback(ptr, h->slotSize);
        return true;
    }

    void runLoop() {
        while (running.load(std::memory_order_relaxed)) {
            bool delivered = false;
            for (int i = 0; i < 64 && running.load(std::memory_order_relaxed); ++i) {
                if (!tryConsumeOne()) break;
                delivered = true;
            }
            if (!running.load(std::memory_order_relaxed)) break;
            if (!delivered) {
                uint32_t us;
                if (creator) { // side A
                    us = rx.hdr->pollIntervalAUs.load(std::memory_order_relaxed);
                } else {       // side B
                    us = rx.hdr->pollIntervalBUs.load(std::memory_order_relaxed);
                }
                if (us == 0) {
                    cpu_relax();
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(us));
                }
            }
        }
    }

    void startThread() {
        if (running.exchange(true)) return;
        worker = std::thread([this]{ runLoop(); });
    }
    void stopThread() {
        if (!running.exchange(false)) return;
        if (worker.joinable()) worker.join();
    }

    ~Impl() {
        stopThread();
        if (tx.hdr) tx.hdr->shutdown = 1;
        if (rx.hdr) rx.hdr->shutdown = 1;
    }
};

// Public API changes
ShmFixedSlotDuplexTransport::ShmFixedSlotDuplexTransport(const std::string& name,
                                                         uint64_t slotSize,
                                                         uint64_t slotCount,
                                                         std::function<void(const uint8_t*, uint64_t)> callback,
                                                         std::chrono::microseconds initialPoll)
    : p_(new Impl(name,
                  slotSize,
                  slotCount,
                  /*forceTruncate=*/true,   // always truncate for side A
                  std::move(callback),
                  initialPoll)) {}

ShmFixedSlotDuplexTransport::ShmFixedSlotDuplexTransport(Impl* impl) : p_(impl) {}

ShmFixedSlotDuplexTransport
ShmFixedSlotDuplexTransport::open(const std::string& name,
                                  std::chrono::milliseconds wait,
                                  std::function<void(const uint8_t*, uint64_t)> callback,
                                  std::chrono::microseconds initialPoll) {
    return ShmFixedSlotDuplexTransport(
        new Impl(name, wait, std::move(callback), initialPoll));
}

ShmFixedSlotDuplexTransport::~ShmFixedSlotDuplexTransport() { delete p_; }

ShmFixedSlotDuplexTransport::ShmFixedSlotDuplexTransport(ShmFixedSlotDuplexTransport&& o) noexcept
    : p_(o.p_) { o.p_ = nullptr; }

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

void ShmFixedSlotDuplexTransport::setLocalSidePollInterval(std::chrono::microseconds us) {
    if (!p_ || !p_->rx.hdr || !p_->tx.hdr) return;
    uint32_t v = static_cast<uint32_t>(us.count());
    if (p_->creator) {
        p_->rx.hdr->pollIntervalAUs.store(v, std::memory_order_relaxed);
        p_->tx.hdr->pollIntervalAUs.store(v, std::memory_order_relaxed);
    } else {
        p_->rx.hdr->pollIntervalBUs.store(v, std::memory_order_relaxed);
        p_->tx.hdr->pollIntervalBUs.store(v, std::memory_order_relaxed);
    }
}

void ShmFixedSlotDuplexTransport::setRemoteSidePollInterval(std::chrono::microseconds us) {
    if (!p_ || !p_->rx.hdr || !p_->tx.hdr) return;
    uint32_t v = static_cast<uint32_t>(us.count());
    if (p_->creator) {
        // remote is B
        p_->rx.hdr->pollIntervalBUs.store(v, std::memory_order_relaxed);
        p_->tx.hdr->pollIntervalBUs.store(v, std::memory_order_relaxed);
    } else {
        // remote is A
        p_->rx.hdr->pollIntervalAUs.store(v, std::memory_order_relaxed);
        p_->tx.hdr->pollIntervalAUs.store(v, std::memory_order_relaxed);
    }
}

std::chrono::microseconds ShmFixedSlotDuplexTransport::getLocalSidePollInterval() const {
    if (!p_ || !p_->rx.hdr) return std::chrono::microseconds{0};
    uint32_t v = p_->creator
        ? p_->rx.hdr->pollIntervalAUs.load(std::memory_order_relaxed)
        : p_->rx.hdr->pollIntervalBUs.load(std::memory_order_relaxed);
    return std::chrono::microseconds{v};
}

std::chrono::microseconds ShmFixedSlotDuplexTransport::getRemoteSidePollInterval() const {
    if (!p_ || !p_->rx.hdr) return std::chrono::microseconds{0};
    uint32_t v = p_->creator
        ? p_->rx.hdr->pollIntervalBUs.load(std::memory_order_relaxed)
        : p_->rx.hdr->pollIntervalAUs.load(std::memory_order_relaxed);
    return std::chrono::microseconds{v};
}

void ShmFixedSlotDuplexTransport::remove(const std::string& name) {
    bip::shared_memory_object::remove((name + ".fs.a2b").c_str());
    bip::shared_memory_object::remove((name + ".fs.b2a").c_str());
}

} // namespace capnproto_shm_transport

