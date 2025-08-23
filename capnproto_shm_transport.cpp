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
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <algorithm>
#include <thread>
#include <atomic>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace bip = boost::interprocess;

namespace capnproto_shm_transport {

std::string version() { return "0.1.0"; }

// ===================== Fixed-slot transport =====================

namespace {

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
	_mm_pause();
#elif defined(__aarch64__)
	asm volatile("yield" ::: "memory");
#else
	// fallback
#endif
}

// Align to cacheline to reduce false sharing between head and tail updates
struct alignas(64) SlotHeader {
	std::size_t slotSize{0};
	std::size_t slotCount{0};
	std::size_t slotMask{0}; // slotCount - 1 (requires power of two)
	std::atomic<std::size_t> head{0}; // next index to read
	char pad0[64 - sizeof(std::atomic<std::size_t>) % 64];
	std::atomic<std::size_t> tail{0}; // next index to write
	char pad1[64 - sizeof(std::atomic<std::size_t>) % 64];
	std::atomic<bool> shutdown{false};
};

struct SlotLayout { SlotHeader* hdr; uint8_t* buf; };

inline std::size_t slots_used(const SlotHeader* h) {
	// relaxed is sufficient for a monotonic snapshot of occupancy
	std::size_t head = h->head.load(std::memory_order_relaxed);
	std::size_t tail = h->tail.load(std::memory_order_relaxed);
	return (tail - head) & h->slotMask;
}

SlotLayout init_or_open_slot_region(bip::managed_shared_memory& shm,
															std::size_t slotSize,
															std::size_t slotCount,
															bool create) {
	SlotHeader* hdr = nullptr;
	uint8_t* data = nullptr;
	if (create) {
		// Require power-of-two slotCount for fast masking
		if ((slotCount & (slotCount - 1)) != 0) {
			throw std::runtime_error("slotCount must be a power of two");
		}
		hdr = shm.construct<SlotHeader>("hdr")();
		hdr->slotSize = slotSize;
		hdr->slotCount = slotCount;
		hdr->slotMask = slotCount - 1;
		hdr->head.store(0, std::memory_order_relaxed);
		hdr->tail.store(0, std::memory_order_relaxed);
		hdr->shutdown.store(false, std::memory_order_relaxed);
		data = shm.construct<uint8_t>("buf")[slotSize * slotCount]();
	} else {
		auto hfound = shm.find<SlotHeader>("hdr");
		auto dfound = shm.find<uint8_t>("buf");
		if (!hfound.first || !dfound.first) throw std::runtime_error("fixed-slot region not initialized");
		if (hfound.first->slotSize != slotSize || hfound.first->slotCount != slotCount)
			throw std::runtime_error("fixed-slot region size mismatch");
		hdr = hfound.first;
		data = dfound.first;
	}
	return {hdr, data};
}

bool slot_write(SlotLayout lay, const uint8_t* src,
				 std::chrono::milliseconds timeout) {
	SlotHeader* h = lay.hdr;
	const auto deadline = (timeout < std::chrono::milliseconds{0})
		? std::chrono::steady_clock::time_point::max()
		: std::chrono::steady_clock::now() + timeout;
	for (;;) {
		std::size_t head = h->head.load(std::memory_order_acquire);
		std::size_t tail = h->tail.load(std::memory_order_relaxed);
		// one slot reserved empty: available = slotCount - used - 1
		if (((tail - head) & h->slotMask) != (h->slotMask)) {
			// there is space
			std::size_t idx = tail & h->slotMask;
			std::memcpy(lay.buf + idx * h->slotSize, src, h->slotSize);
			// publish write
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
	SlotHeader* h = lay.hdr;
	const auto deadline = (timeout < std::chrono::milliseconds{0})
		? std::chrono::steady_clock::time_point::max()
		: std::chrono::steady_clock::now() + timeout;
	for (;;) {
		std::size_t head = h->head.load(std::memory_order_relaxed);
		std::size_t tail = h->tail.load(std::memory_order_acquire);
		if (((tail - head) & h->slotMask) != 0) {
			std::size_t idx = head & h->slotMask;
			std::memcpy(dst, lay.buf + idx * h->slotSize, h->slotSize);
			// consume
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
	bool isA;
	std::size_t slotSize;
	std::size_t slotCount;

	std::unique_ptr<bip::managed_shared_memory> seg_tx;
	std::unique_ptr<bip::managed_shared_memory> seg_rx;
	SlotLayout tx{};
	SlotLayout rx{};

	Impl(const std::string& base, std::size_t sSize, std::size_t sCount, bool sideA,
		 bool openOrCreate, bool truncateOnCreate)
		: name(base), isA(sideA), slotSize(sSize), slotCount(sCount) {
		std::string a2b = base + ".fs.a2b";
		std::string b2a = base + ".fs.b2a";
		auto create_seg = [&](const std::string& segName) {
			if (truncateOnCreate) bip::shared_memory_object::remove(segName.c_str());
			const std::size_t bytes = sizeof(SlotHeader) + slotSize * slotCount + 4096;
			return std::make_unique<bip::managed_shared_memory>(bip::create_only, segName.c_str(), bytes);
		};
		if (isA) {
			seg_tx = create_seg(a2b);
			seg_rx = create_seg(b2a);
			tx = init_or_open_slot_region(*seg_tx, slotSize, slotCount, /*create=*/true);
			rx = init_or_open_slot_region(*seg_rx, slotSize, slotCount, /*create=*/true);
		} else {
			try {
				seg_tx = std::make_unique<bip::managed_shared_memory>(bip::open_only, b2a.c_str());
				seg_rx = std::make_unique<bip::managed_shared_memory>(bip::open_only, a2b.c_str());
				tx = init_or_open_slot_region(*seg_tx, slotSize, slotCount, /*create=*/false);
				rx = init_or_open_slot_region(*seg_rx, slotSize, slotCount, /*create=*/false);
			} catch (...) {
				seg_tx = create_seg(b2a);
				seg_rx = create_seg(a2b);
				tx = init_or_open_slot_region(*seg_tx, slotSize, slotCount, /*create=*/true);
				rx = init_or_open_slot_region(*seg_rx, slotSize, slotCount, /*create=*/true);
			}
		}
	}

	~Impl() {
		for (auto lay : {tx, rx}) {
			if (lay.hdr) {
				lay.hdr->shutdown.store(true, std::memory_order_relaxed);
			}
		}
	}
};

ShmFixedSlotDuplexTransport::ShmFixedSlotDuplexTransport(const std::string& name,
		std::size_t slotSize,
		std::size_t slotCount,
		bool isSideA,
		bool openOrCreate,
		bool truncateOnCreate)
	: p_(new Impl(name, slotSize, slotCount, isSideA, openOrCreate, truncateOnCreate)) {}

ShmFixedSlotDuplexTransport::~ShmFixedSlotDuplexTransport() { delete p_; }

ShmFixedSlotDuplexTransport::ShmFixedSlotDuplexTransport(ShmFixedSlotDuplexTransport&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
ShmFixedSlotDuplexTransport& ShmFixedSlotDuplexTransport::operator=(ShmFixedSlotDuplexTransport&& o) noexcept {
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
	auto calc = [](SlotLayout lay) -> SlotRingStats {
		SlotRingStats s{};
		if (!lay.hdr) return s;
	s.slotSize = lay.hdr->slotSize;
	s.slotCount = lay.hdr->slotCount;
	s.usedSlots = slots_used(lay.hdr);
	s.shutdown = lay.hdr->shutdown.load(std::memory_order_relaxed);
		return s;
	};
	out.tx = calc(p_->tx);
	out.rx = calc(p_->rx);
	return true;
}

void ShmFixedSlotDuplexTransport::remove(const std::string& name) {
	bip::shared_memory_object::remove((name + ".fs.a2b").c_str());
	bip::shared_memory_object::remove((name + ".fs.b2a").c_str());
}

} // namespace capnproto_shm_transport

