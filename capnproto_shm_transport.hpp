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
#include <functional>
#include <chrono>
#include <vector>
#include <cstdint>
#include <string>

namespace capnproto_shm_transport {

class ShmFixedSlotDuplexTransport {
public:
    ShmFixedSlotDuplexTransport(const std::string& name,
                                uint64_t slotSize,
                                uint64_t slotCount,
                                std::function<void(const uint8_t*, uint64_t)> callback,
                                std::chrono::microseconds initialPoll = std::chrono::milliseconds(1));

    static ShmFixedSlotDuplexTransport open(const std::string& name,
                                            std::chrono::milliseconds wait,
                                            std::function<void(const uint8_t*, uint64_t)> callback,
                                            std::chrono::microseconds initialPoll = std::chrono::milliseconds(1));

    static ShmFixedSlotDuplexTransport open(const std::string& name,
                                            std::chrono::milliseconds wait) {
        return open(name, wait, nullptr);
    }

    // Move-only
    ~ShmFixedSlotDuplexTransport();
    ShmFixedSlotDuplexTransport(ShmFixedSlotDuplexTransport&&) noexcept;
    ShmFixedSlotDuplexTransport& operator=(ShmFixedSlotDuplexTransport&&) noexcept;
    ShmFixedSlotDuplexTransport(const ShmFixedSlotDuplexTransport&) = delete;
    ShmFixedSlotDuplexTransport& operator=(const ShmFixedSlotDuplexTransport&) = delete;

    uint32_t sendSlot(const uint8_t* data, uint64_t len, std::chrono::milliseconds timeout);
    uint32_t recvSlot(std::vector<uint8_t>& out, std::chrono::milliseconds timeout);

    struct SlotRingStats {
        uint64_t slotSize{0};
        uint64_t slotCount{0};
        uint64_t usedSlots{0};
        uint32_t shutdown{0};
    };
    struct SlotTransportStats { SlotRingStats tx; SlotRingStats rx; };
    uint32_t getStats(SlotTransportStats& out);

    uint64_t slotSize()  const noexcept;
    uint64_t slotCount() const noexcept;
    uint32_t isCreator() const noexcept;

    void setLocalSidePollInterval(std::chrono::microseconds us);
    void setRemoteSidePollInterval(std::chrono::microseconds us);
    std::chrono::microseconds getLocalSidePollInterval()  const;
    std::chrono::microseconds getRemoteSidePollInterval() const;

    static void remove(const std::string& name);

private:
    struct Impl;
    explicit ShmFixedSlotDuplexTransport(Impl* impl);
    Impl* p_{nullptr};
};

std::string version();

} // namespace capnproto_shm_transport

