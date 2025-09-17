# capnproto_shm_transport

A minimal, high‑performance, fixed‑size, duplex shared memory transport (A ↔ B) using Boost.Interprocess.  
Designed for two processes exchanging uniformly sized messages with low latency and tunable (very low) CPU usage.

---

## Core Concept

Side A (the “master”) creates a fresh channel every run. It always deletes (truncates) any stale shared memory segments before creating new ones.  
Side B (the “follower”) only opens an existing channel; it never truncates.

Allocated shared memory segments:

| Segment         | Direction | Writer | Reader |
|-----------------|-----------|--------|--------|
| `<name>.fs.a2b` | A → B     | A      | B      |
| `<name>.fs.b2a` | B → A     | B      | A      |

Each segment holds:
1. SlotHeader (slot meta, indices, side A/B poll intervals, flags)
2. Ring buffer: `slotCount` fixed-size slots (`slotSize` bytes each)

Single producer + single consumer per direction → no locks.

---

## Features

- Duplex, bi‑directional fixed-size message passing
- Lock‑free ring (power‑of‑two slots)
- Background receive thread via callback (optional)
- Dynamic per‑side (A/B) idle poll interval (µs) stored in shared memory
- Change your own or remote side’s poll interval live
- Latency/CPU trade‑off: 0 µs (busy spin) to N µs sleep
- Graceful shutdown flag
- Simple success (1) / fail (0) returns (timeouts, shutdown, mismatch)
- Portable (Linux/macOS/Windows) with Boost.Interprocess
- Automatic truncation policy: A always refreshes; B never destroys

---

## Poll Interval Model (Per Side)

Two independent shared fields:
- `pollIntervalAUs` → side A’s idle sleep
- `pollIntervalBUs` → side B’s idle sleep

APIs:
- `setLocalSidePollInterval(us)` – adjust this process’ receive loop
- `setRemoteSidePollInterval(us)` – request remote side to adopt new idle sleep
- `getLocalSidePollInterval()` / `getRemoteSidePollInterval()`

Values:
- `0` → busy spin (lowest latency, highest CPU)
- `>0` µs → sleep that long when idle after draining a small burst

---

## Public API (Essentials)

```cpp
class ShmFixedSlotDuplexTransport {
public:
    // Side A (creator): always truncates any old segments of the same name.
    ShmFixedSlotDuplexTransport(const std::string& name,
                                uint64_t slotSize,
                                uint64_t slotCount,              // power-of-two
                                std::function<void(const uint8_t*, uint64_t)> callback,
                                std::chrono::microseconds initialPoll = std::chrono::milliseconds(1));

    // Side B (opener): waits up to 'wait' for A.
    static ShmFixedSlotDuplexTransport open(const std::string& name,
                                            std::chrono::milliseconds wait,
                                            std::function<void(const uint8_t*, uint64_t)> callback,
                                            std::chrono::microseconds initialPoll = std::chrono::milliseconds(1));

    // Open without callback (no background thread).
    static ShmFixedSlotDuplexTransport open(const std::string& name,
                                            std::chrono::milliseconds wait);

    uint32_t sendSlot(const uint8_t* data, uint64_t len, std::chrono::milliseconds timeout);
    uint32_t recvSlot(std::vector<uint8_t>& out, std::chrono::milliseconds timeout);

    struct SlotRingStats {
        uint64_t slotSize;
        uint64_t slotCount;
        uint64_t usedSlots;
        uint32_t shutdown;
    };
    struct SlotTransportStats { SlotRingStats tx; SlotRingStats rx; };
    uint32_t getStats(SlotTransportStats& out);

    uint64_t slotSize()  const noexcept;
    uint64_t slotCount() const noexcept;
    uint32_t isCreator() const noexcept; // 1 = side A, 0 = side B

    // Dynamic poll control (per side, reflected in both headers)
    void setLocalSidePollInterval(std::chrono::microseconds us);
    void setRemoteSidePollInterval(std::chrono::microseconds us);
    std::chrono::microseconds getLocalSidePollInterval()  const;
    std::chrono::microseconds getRemoteSidePollInterval() const;

    static void remove(const std::string& name); // Full removal (normally only A does this)
};
```

### Callback Semantics

If a non-empty callback is provided:
- A background thread starts immediately.
- It drains up to a burst (e.g. 64 messages) each loop.
- If no messages were delivered, it sleeps for the current side’s poll interval.
- If poll interval is 0, it busy-spins (with short relax instructions).
- Memory passed to callback is re-used; copy out if needed later.

### Return Values

`sendSlot` / `recvSlot` return:
- `1` on success
- `0` on timeout, shutdown, or invalid length (for send)

### Timeouts

Provide `std::chrono::milliseconds{-1}` for an infinite wait.
A finite timeout causes the operation to return `0` if no slot state change before deadline.

### Shutdown Behavior

Destructor sets a shutdown flag; blocked counterpart operations will quickly return `0`.

---

## Typical Usage

### Side A (Creator)

```cpp
using namespace capnproto_shm_transport;

// Optional: ensure a clean start.
ShmFixedSlotDuplexTransport::remove("demo_channel");

ShmFixedSlotDuplexTransport a(
    "demo_channel",
    1024,          // slotSize
    1024,          // slotCount (power-of-two)
    [](const uint8_t* data, uint64_t len) {
        // Handle incoming from side B
        (void)data; (void)len;
    },
    std::chrono::microseconds{1000} // initial poll (1ms)
);

std::vector<uint8_t> slot(a.slotSize());
uint64_t seq = 0;
while (/* condition */) {
    std::memcpy(slot.data(), &seq, sizeof(seq));
    if (!a.sendSlot(slot.data(), slot.size(), std::chrono::milliseconds{-1}))
        break;
    ++seq;
}
```

### Side B (Opener)

```cpp
auto b = ShmFixedSlotDuplexTransport::open(
    "demo_channel",
    std::chrono::milliseconds{500},
    [](const uint8_t* data, uint64_t len) {
        // Handle incoming from A
        (void)data; (void)len;
    },
    std::chrono::microseconds{1000}
);

std::vector<uint8_t> slot(b.slotSize());
uint64_t seq = 0;
while (/* condition */) {
    std::memcpy(slot.data(), &seq, sizeof(seq));
    if (!b.sendSlot(slot.data(), slot.size(), std::chrono::milliseconds{-1}))
        break;
    ++seq;
}
```

### Adjust Poll Intervals at Runtime

```cpp
// Lower our own poll to 200µs (faster reaction, more CPU)
a.setLocalSidePollInterval(std::chrono::microseconds{200});

// Ask remote side to slow to 2ms (less CPU there)
a.setRemoteSidePollInterval(std::chrono::microseconds{2000});

std::cout << "Local="  << a.getLocalSidePollInterval().count()
          << "us Remote=" << a.getRemoteSidePollInterval().count() << "us\n";
```

### Using Synchronous Receive (No Callback)

```cpp
auto b = ShmFixedSlotDuplexTransport::open("demo_channel", std::chrono::milliseconds{500});
std::vector<uint8_t> out;
if (b.recvSlot(out, std::chrono::milliseconds{10})) {
    // process 'out'
}
```

---

## Poll Interval Trade-Offs

| Poll Interval | CPU Usage (Idle) | Latency Added (Typical) |
|---------------|------------------|-------------------------|
| 0 µs          | High (busy)      | ~tens of µs             |
| 50–200 µs     | Low-Moderate     | Sub-millisecond         |
| 1000 µs       | Very Low         | ~1 ms typical           |
| >1 ms         | Minimal CPU      | Roughly that interval   |

You can adapt live depending on traffic: busy spin during bursts, relax when quiet.

---

## Performance & Profiling

Build with profiling flags (see `.bazelrc` `--config=profile` already provided):

```bash
bazel build --config=profile //:capnproto_shm_transport_demo
perf stat -d -d ./bazel-bin/capnproto_shm_transport_demo --a 5 1024 1024
perf record -F 999 -g -- ./bazel-bin/capnproto_shm_transport_demo --b 10
perf report
```

Quick CPU % approximation inside the demo is printed when using supplied `main.cpp` (user+sys vs wall).

---

## Demo Program (main.cpp Modes)

| Mode        | Arguments (Creator / Opener)                  | Behavior                          |
|-------------|-----------------------------------------------|-----------------------------------|
| `--a`       | seconds slotSize slotCount [local] [remote]   | Burst send (A)                    |
| `--b`       | seconds [local] [remote]                      | Burst send (B)                    |
| `--a30`     | seconds slotSize slotCount [local] [remote]   | 1 message / 30 ms pacing (A)     |
| `--b30`     | seconds [local] [remote]                      | 1 message / 30 ms pacing (B)     |

`local` = initial local poll interval (µs)  
`remote` = (optional) ask the other side to adopt this poll (µs)

Both sides still receive (callback) and send their own test traffic.

---

## Building (Bazel)

Default:

```bash
bazel build //:capnproto_shm_transport_demo
```

Musl static (if configured):

```bash
bazel build --config=musl //:capnproto_shm_transport_demo
```

Run:

```bash
./bazel-bin/capnproto_shm_transport_demo --a 10 1024 1024
./bazel-bin/capnproto_shm_transport_demo --b 10
```

---

## Error Handling Summary

| Operation                        | Failure Mode                           | Result |
|---------------------------------|----------------------------------------|--------|
| Constructor (creator)          | Invalid args / allocation failure       | throws |
| open() (opener)                | Timeout waiting for ready              | throws |
| sendSlot / recvSlot            | Timeout / remote shutdown / mismatch   | 0      |
| getStats                       | Invalid internal state                 | 0      |
| Destructor                     | Never throws                           | n/a    |

---

## Portability Notes

- Relies on Boost.Interprocess; choose names without slashes for Windows.
- `cpu_relax()` uses architecture-specific pause/yield with fallback.
- For sub-millisecond sleeps on Windows you may need system timer resolution tweaks (not included).

---

## When To Use This

Use it when you need:
- Two processes
- High frequency or bounded-latency message passing
- Fixed message size known upfront
- Ability to tune CPU vs latency easily

Not ideal if:
- You need variable/large messages (would need framing layer)
- You require multi-producer per direction (would need additional coordination)
- You need more than two peers (would require hub or redesign)

---

## Extensibility Ideas

- Support variable-size frames (length prefix inside slot)
- Add eventfd/futex to reduce wakeups further
- NUMA placement hints
- Cap’n Proto schema aware wrapper (zero-copy decode from slot)
- Multi-producer adaptation (ticket reservation)

---

## License

GPLv3 (see headers). Ensure compatibility with your downstream usage.

---

## Quick Checklist

| Need | Supported |
|------|-----------|
| Duplex fixed-size slots | ✔ |
| Single producer/consumer per direction | ✔ |
| Runtime poll tuning (A/B) | ✔ |
| Latency <1 ms (proper poll config) | ✔ |
| Dynamic removal / cleanup | ✔ |
| Portable (major desktop OS) | ✔ |

---

Questions or improvements welcome. Adjust poll intervals at runtime to explore latency/CPU trade-offs.
