// Implementation of shared-memory duplex transport using Boost.Interprocess.
#include "capnproto_shm_transport.hpp"

#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <algorithm>
#include <thread>

namespace bip = boost::interprocess;

namespace capnproto_shm_transport {

std::string version() { return "0.1.0"; }

namespace {
constexpr std::size_t kAlign = alignof(std::max_align_t);

inline std::size_t align_up(std::size_t n, std::size_t a = kAlign) {
	return (n + (a - 1)) & ~(a - 1);
}

struct RingHeader {
	bip::interprocess_mutex mtx;
	bip::interprocess_condition can_read;
	bip::interprocess_condition can_write;
	std::size_t capacity; // usable bytes in the buffer
	std::size_t head;     // read position
	std::size_t tail;     // write position
	bool shutdown;
};

// Layout in SHM: [RingHeader][padding][byte buffer of size capacity]
struct ShmLayout { RingHeader* hdr; uint8_t* buf; };

ShmLayout init_or_open_region(bip::managed_shared_memory& shm,
															std::size_t capacity,
															bool create) {
	RingHeader* hdr = nullptr;
	uint8_t* data = nullptr;

	if (create) {
		hdr = shm.construct<RingHeader>("hdr")();
		hdr->capacity = capacity;
		hdr->head = 0;
		hdr->tail = 0;
		hdr->shutdown = false;
		data = shm.construct<uint8_t>("buf")[capacity]();
	} else {
		auto hfound = shm.find<RingHeader>("hdr");
		auto dfound = shm.find<uint8_t>("buf");
		if (!hfound.first || !dfound.first || dfound.second < capacity) {
			throw std::runtime_error("shared memory region not initialized");
		}
		hdr = hfound.first;
		data = dfound.first;
	}
	return {hdr, data};
}

bool ring_write(ShmLayout lay, const uint8_t* src, std::size_t len,
								std::chrono::milliseconds timeout) {
	RingHeader* h = lay.hdr;
	bip::scoped_lock<bip::interprocess_mutex> lock(h->mtx);

	auto pred_space = [&]() {
		std::size_t head = h->head;
		std::size_t tail = h->tail;
		std::size_t used = (tail + h->capacity - head) % h->capacity;
		return (h->capacity - used - 1) >= len; // keep one byte gap
	};

	auto deadline = (timeout < std::chrono::milliseconds{0})
											? std::chrono::steady_clock::time_point::max()
											: std::chrono::steady_clock::now() + timeout;
	while (!pred_space() && !h->shutdown) {
		if (std::chrono::steady_clock::now() >= deadline) return false;
		lock.unlock();
		std::this_thread::sleep_for(std::chrono::microseconds(200));
		lock.lock();
	}
	if (h->shutdown) return false;

	// Write with wrap-around
	std::size_t tail = h->tail;
	std::size_t cap = h->capacity;
	std::size_t first = std::min(len, cap - tail);
	std::memcpy(lay.buf + tail, src, first);
	if (len > first) std::memcpy(lay.buf, src + first, len - first);
	h->tail = (tail + len) % cap;
	h->can_read.notify_one();
	return true;
}

bool ring_read(ShmLayout lay, uint8_t* dst, std::size_t len,
			   std::chrono::milliseconds timeout) {
	RingHeader* h = lay.hdr;
	bip::scoped_lock<bip::interprocess_mutex> lock(h->mtx);

	auto pred_data = [&]() {
		std::size_t head = h->head;
		std::size_t tail = h->tail;
		std::size_t avail = (tail + h->capacity - head) % h->capacity;
		return avail >= len;
	};

		auto deadline = (timeout < std::chrono::milliseconds{0})
												? std::chrono::steady_clock::time_point::max()
												: std::chrono::steady_clock::now() + timeout;
		while (!pred_data() && !h->shutdown) {
			if (std::chrono::steady_clock::now() >= deadline) return false;
			lock.unlock();
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			lock.lock();
		}
		if (h->shutdown) return false;

	// Read with wrap-around
	std::size_t head = h->head;
	std::size_t cap = h->capacity;
	std::size_t first = std::min(len, cap - head);
	std::memcpy(dst, lay.buf + head, first);
	if (len > first) std::memcpy(dst + first, lay.buf, len - first);
	h->head = (head + len) % cap;
	h->can_write.notify_one();
	return true;
}

} // namespace

struct ShmDuplexTransport::Impl {
	std::string name;
	bool isA;
	std::size_t capacity;

	// Two segments: a2b and b2a
	std::unique_ptr<bip::managed_shared_memory> seg_tx;
	std::unique_ptr<bip::managed_shared_memory> seg_rx;
	ShmLayout tx{};
	ShmLayout rx{};

		Impl(const std::string& base, std::size_t cap, bool sideA,
				 bool openOrCreate, bool truncateOnCreate)
			: name(base), isA(sideA), capacity(cap) {
		std::string a2b = base + ".a2b";
		std::string b2a = base + ".b2a";

			// Size budget: header + buffer + small overhead.
			auto create_seg = [&](const std::string& segName) {
				if (truncateOnCreate) bip::shared_memory_object::remove(segName.c_str());
				const std::size_t bytes = sizeof(RingHeader) + capacity + 4096;
				return std::make_unique<bip::managed_shared_memory>(bip::create_only, segName.c_str(), bytes);
			};

		// Build both segments. Side A creates if possible.
		if (isA) {
			seg_tx = create_seg(a2b);
			seg_rx = create_seg(b2a);
			tx = init_or_open_region(*seg_tx, cap, /*create=*/true);
			rx = init_or_open_region(*seg_rx, cap, /*create=*/true);
		} else {
			// Try open, fallback create
			try {
				seg_tx = std::make_unique<bip::managed_shared_memory>(bip::open_only, b2a.c_str());
				seg_rx = std::make_unique<bip::managed_shared_memory>(bip::open_only, a2b.c_str());
				tx = init_or_open_region(*seg_tx, cap, /*create=*/false);
				rx = init_or_open_region(*seg_rx, cap, /*create=*/false);
			} catch (...) {
				seg_tx = create_seg(b2a);
				seg_rx = create_seg(a2b);
				tx = init_or_open_region(*seg_tx, cap, /*create=*/true);
				rx = init_or_open_region(*seg_rx, cap, /*create=*/true);
			}
		}
	}

	~Impl() {
		// signal shutdown to any waiters
		for (auto lay : {tx, rx}) {
			if (lay.hdr) {
				bip::scoped_lock<bip::interprocess_mutex> lock(lay.hdr->mtx);
				lay.hdr->shutdown = true;
				lay.hdr->can_read.notify_all();
				lay.hdr->can_write.notify_all();
			}
		}
	}
};

ShmDuplexTransport::ShmDuplexTransport(const std::string& name,
																			 std::size_t capacityBytes,
																			 bool isSideA,
																			 bool openOrCreate,
																			 bool truncateOnCreate)
		: p_(new Impl(name, capacityBytes, isSideA, openOrCreate, truncateOnCreate)) {}

ShmDuplexTransport::~ShmDuplexTransport() { delete p_; }

ShmDuplexTransport::ShmDuplexTransport(ShmDuplexTransport&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
ShmDuplexTransport& ShmDuplexTransport::operator=(ShmDuplexTransport&& o) noexcept {
	if (this != &o) {
		delete p_;
		p_ = o.p_;
		o.p_ = nullptr;
	}
	return *this;
}

bool ShmDuplexTransport::send(const uint8_t* data, std::size_t len,
															std::chrono::milliseconds timeout) {
	// frame: 4-byte little-endian length, then payload
	if (!p_ || len > 0xFFFFFFFFu) return false;
	uint8_t header[4];
	uint32_t L = static_cast<uint32_t>(len);
	header[0] = static_cast<uint8_t>(L & 0xFF);
	header[1] = static_cast<uint8_t>((L >> 8) & 0xFF);
	header[2] = static_cast<uint8_t>((L >> 16) & 0xFF);
	header[3] = static_cast<uint8_t>((L >> 24) & 0xFF);

	auto start = std::chrono::steady_clock::now();
	auto rem = timeout;
	auto adjust = [&](bool ok) {
		if (timeout < std::chrono::milliseconds{0}) return std::chrono::milliseconds{-1};
		auto now = std::chrono::steady_clock::now();
		auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
		if (spent >= timeout) return std::chrono::milliseconds{0};
		return timeout - spent;
	};

	if (!ring_write(p_->tx, header, sizeof(header), rem)) return false;
	rem = adjust(true);
	if (timeout >= std::chrono::milliseconds{0} && rem.count() <= 0) return false;
	if (len == 0) return true;
	return ring_write(p_->tx, data, len, rem);
}

bool ShmDuplexTransport::recv(std::vector<uint8_t>& out,
															std::chrono::milliseconds timeout) {
	if (!p_) return false;
	uint8_t header[4];
	if (!ring_read(p_->rx, header, sizeof(header), timeout)) return false;
	uint32_t L = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
							 ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
	out.resize(L);
	if (L == 0) return true;
	return ring_read(p_->rx, out.data(), L, timeout);
}

bool ShmDuplexTransport::sendWords(const void* words, std::size_t wordCount,
																	 std::chrono::milliseconds timeout) {
	const uint8_t* bytes = static_cast<const uint8_t*>(words);
	return send(bytes, wordCount * sizeof(uint64_t), timeout);
}

bool ShmDuplexTransport::recvWords(std::vector<uint64_t>& outWords,
																	 std::chrono::milliseconds timeout) {
	std::vector<uint8_t> bytes;
	if (!recv(bytes, timeout)) return false;
	if (bytes.size() % sizeof(uint64_t) != 0) return false;
	outWords.resize(bytes.size() / sizeof(uint64_t));
	std::memcpy(outWords.data(), bytes.data(), bytes.size());
	return true;
}

void ShmDuplexTransport::remove(const std::string& name) {
	bip::shared_memory_object::remove((name + ".a2b").c_str());
	bip::shared_memory_object::remove((name + ".b2a").c_str());
}

} // namespace capnproto_shm_transport

