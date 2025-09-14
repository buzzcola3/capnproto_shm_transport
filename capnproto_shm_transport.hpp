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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace capnproto_shm_transport {

struct SlotRingStats {
    std::size_t slotSize{0};
    std::size_t slotCount{0};
    std::size_t usedSlots{0};
    bool shutdown{false};
};

struct SlotTransportStats {
    SlotRingStats tx;
    SlotRingStats rx;
};

class ShmFixedSlotDuplexTransport {
public:
    // Creator (Side A): creates both directions. slotCount must be power of two.
    ShmFixedSlotDuplexTransport(const std::string& name,
                                std::size_t slotSize,
                                std::size_t slotCount,
                                bool truncateOnCreate = true);

    // Open existing (Side B): waits (spin) until creator published headers or timeout.
    static ShmFixedSlotDuplexTransport open(const std::string& name,
                                            std::chrono::milliseconds wait = std::chrono::milliseconds{5000});

    ShmFixedSlotDuplexTransport(ShmFixedSlotDuplexTransport&&) noexcept;
    ShmFixedSlotDuplexTransport& operator=(ShmFixedSlotDuplexTransport&&) noexcept;
    ~ShmFixedSlotDuplexTransport();

    bool sendSlot(const uint8_t* data, std::size_t len,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});
    bool recvSlot(std::vector<uint8_t>& out,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

    bool getStats(SlotTransportStats& out);

    std::size_t slotSize()  const noexcept;
    std::size_t slotCount() const noexcept;
    bool isCreator() const noexcept;

    static void remove(const std::string& name);

private:
    struct Impl;
    explicit ShmFixedSlotDuplexTransport(Impl* impl); // used by open()
    Impl* p_{nullptr};
};

std::string version();

} // namespace capnproto_shm_transport

