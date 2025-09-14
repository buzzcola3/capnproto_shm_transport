# capnproto_shm_transport

Ultra low‑latency shared memory duplex transport (fixed-size slots, lock‑free SPSC per direction) for two processes. Designed for embedding Cap’n Proto messages (just copy serialized bytes into a slot). Licensed under GPL-3.0-or-later.

## Features
- Two unidirectional rings: A→B and B→A (duplex).
- Fixed slot size & power‑of‑two slot count (mask wrap).
- Lock‑free single producer / single consumer per direction.
- Spin + cpu_relax() for minimal latency.
- Simple API: creator (A) supplies sizes, opener (B) discovers them.

## Build
```bash
bazel build //:capnproto_shm_transport
```

## Demo Tool
Creator side (A) sends as fast as possible for N seconds:
```bash
bazel run //:capnproto_shm_transport_demo -- --create <name> <seconds> <slotSize> <slotCount>
# example:
bazel run //:capnproto_shm_transport_demo -- --create capnp_ipc 3 256 1024
```

Opener side (B) receives for N seconds:
```bash
bazel run //:capnproto_shm_transport_demo -- --open <name> <seconds>
# example (run in another terminal before creator finishes):
bazel run //:capnproto_shm_transport_demo -- --open capnp_ipc 3
```

Output (both modes):
```
pps=543210
```

Only the creator removes the shared memory segments at the end.

## API (minimal)
```cpp
#include "capnproto_shm_transport.hpp"
using namespace capnproto_shm_transport;

ShmFixedSlotDuplexTransport a("capnp_ipc", /*slotSize=*/256, /*slotCount=*/1024, /*truncateOnCreate=*/true);
auto b = ShmFixedSlotDuplexTransport::open("capnp_ipc");

std::vector<uint8_t> buf(256);
a.sendSlot(buf.data(), buf.size());        // blocks (infinite timeout)
std::vector<uint8_t> rx;
b.recvSlot(rx);                            // blocks (infinite timeout)
```

Notes:
- slotCount must be a power of two (e.g. 256, 512, 1024…).
- sendSlot len must equal slotSize.
- Infinite timeout: pass std::chrono::milliseconds{-1}.
- For Cap’n Proto: build message into a fixed-size buffer matching slotSize.

## Bazel Integration (bzlmod)
In consumer MODULE.bazel:
```bzl
bazel_dep(name = "capnproto_shm_transport", version = "0.0.1")
git_override(
    module_name = "capnproto_shm_transport",
    remote = "https://github.com/youruser/capnproto_shm_transport.git",
    tag = "v0.0.1",
)
```

In your BUILD file:
```bzl
cc_binary(
    name = "my_app",
    srcs = ["main.cpp"],
    deps = ["@capnproto_shm_transport//:capnproto_shm_transport"],
)
```

Include:
```cpp
#include "capnproto_shm_transport.hpp"
```

## Performance Tips
- Use small slotSize for high message rate; larger for bigger payloads.
- Pin processes/threads (taskset) for lower jitter.
- Ensure /dev/shm has enough space: slotSize * slotCount * 2 segments (+ overhead).

## License
GPL-3.0-or-later (see LICENSE). Add SPDX tag in your sources if desired:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
```

## Version
`version()` returns
