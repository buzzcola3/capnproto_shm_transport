// Shared-memory Cap'n Proto transport built on Boost.Interprocess.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace capnproto_shm_transport {

// Simple version identifier.
std::string version();

// A duplex shared-memory transport optimized for fixed-size slots.
	// Single-producer single-consumer per direction, lock-free with atomics.
	// Each message occupies exactly one slot of size slotSize bytes (caller must
	// provide exactly slotSize bytes when sending; receiver returns buffers of that size).
	// Note: slotCount must be a power of two for optimal performance.
	class ShmFixedSlotDuplexTransport {
	public:
		ShmFixedSlotDuplexTransport(const std::string& name,
																std::size_t slotSize,
																std::size_t slotCount,
																bool isSideA,
																bool openOrCreate = true,
																bool truncateOnCreate = false);

		~ShmFixedSlotDuplexTransport();

		ShmFixedSlotDuplexTransport(const ShmFixedSlotDuplexTransport&) = delete;
		ShmFixedSlotDuplexTransport& operator=(const ShmFixedSlotDuplexTransport&) = delete;
		ShmFixedSlotDuplexTransport(ShmFixedSlotDuplexTransport&&) noexcept;
		ShmFixedSlotDuplexTransport& operator=(ShmFixedSlotDuplexTransport&&) noexcept;

		// Send exactly one slot; len must equal slotSize.
		bool sendSlot(const uint8_t* data,
									std::size_t len,
									std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

		// Receive exactly one slot; out will be resized to slotSize.
		bool recvSlot(std::vector<uint8_t>& out,
									std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

		struct SlotRingStats {
			std::size_t slotSize{0};
			std::size_t slotCount{0};
			std::size_t usedSlots{0};
			bool shutdown{false};
		};
		struct SlotTransportStats { SlotRingStats tx; SlotRingStats rx; };
		bool getStats(SlotTransportStats& out);

		static void remove(const std::string& name); // removes only the fixed-slot segments

	private:
		struct Impl;
		Impl* p_{};
	};

} // namespace capnproto_shm_transport

