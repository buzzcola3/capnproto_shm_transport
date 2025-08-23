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

// A duplex shared-memory message transport using two ring buffers
// (A->B and B->A) in separate shared memory segments. Messages are
// framed as [uint32_le length][payload bytes]. Designed to carry
// Cap'n Proto message bytes (word-aligned recommended), but agnostic
// to the payload format.
//
// Usage:
//   // Side A:
//   ShmDuplexTransport t{"my-transport", 1<<20, true};
//   t.send({data, data+len});
//   std::vector<uint8_t> msg; t.recv(msg);
//
//   // Side B:
//   ShmDuplexTransport t{"my-transport", 1<<20, false};
//   ...
class ShmDuplexTransport {
public:
	// Create or open two shared-memory ring buffers with a base name.
	// If isSideA is true, TX uses "<name>.a2b" and RX uses "<name>.b2a".
	// Otherwise the opposite.
	ShmDuplexTransport(const std::string& name,
										 std::size_t capacityBytes,
										 bool isSideA,
										 bool openOrCreate = true,
										 bool truncateOnCreate = false);

	~ShmDuplexTransport();

	ShmDuplexTransport(const ShmDuplexTransport&) = delete;
	ShmDuplexTransport& operator=(const ShmDuplexTransport&) = delete;
	ShmDuplexTransport(ShmDuplexTransport&&) noexcept;
	ShmDuplexTransport& operator=(ShmDuplexTransport&&) noexcept;

	// Send a message (blocking). Returns false on timeout or error.
	bool send(const uint8_t* data, std::size_t len,
						std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

	// Convenience: send from a byte container
	bool send(const std::vector<uint8_t>& bytes,
						std::chrono::milliseconds timeout = std::chrono::milliseconds{-1}) {
		return send(bytes.data(), bytes.size(), timeout);
	}

	// Receive a message (blocking). Returns false on timeout or error.
	bool recv(std::vector<uint8_t>& out,
						std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

	// Helpers for Cap'n Proto word buffers (no capnp headers needed)
	bool sendWords(const void* words, std::size_t wordCount,
								 std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});
	bool recvWords(std::vector<uint64_t>& outWords,
								 std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

	// Remove shared memory segments (use with care; typically done on cleanup).
	static void remove(const std::string& name);

private:
	struct Impl;
	Impl* p_{}; // Pimpl to keep header small and independent of Boost headers.
};

} // namespace capnproto_shm_transport

