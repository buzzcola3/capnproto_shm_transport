# capnproto_shm_transport

Ultra-low-latency Cap'n Proto transport over POSIX shared memory. Two SPSC lock-free rings (A→B and B→A) with fixed-size slots.

- Fixed-size slots, SPSC per direction, lock-free (atomics)
- Names: `<name>.fs.a2b` and `<name>.fs.b2a`
- slotCount must be a power of two

## Build

```bash
bazel build //:capnproto_shm_transport //:capnproto_shm_transport_demo
```

## Run

Throughput:
```bash
bazel run //:capnproto_shm_transport_demo -- 5 64 1024    # prints: pps=<value>
```

Drain (prefill, then measure drain time):
```bash
bazel run //:capnproto_shm_transport_demo -- --drain 64 1024  # prints: drain_us=.. drain_pps=..
```

## Use in another Bazel project (bzlmod)

`MODULE.bazel`:
```starlark
bazel_dep(name = "capnproto_shm_transport", version = "0.0.1")
git_override(
    module_name = "capnproto_shm_transport",
    remote = "https://github.com/you/capnproto_shm_transport.git",
    tag = "v0.0.1",
)
```

`BUILD.bazel`:
```starlark
deps = ["@capnproto_shm_transport//:capnproto_shm_transport"]
```

## API (quick)

Header: `capnproto_shm_transport.hpp`

- `ShmFixedSlotDuplexTransport(name, slotSize, slotCount, isSideA, openOrCreate=true, truncateOnCreate=false)`
- `sendSlot(data, len=slotSize, timeout=-1)`
- `recvSlot(out, timeout=-1)`
- `getStats(stats)`
- `ShmFixedSlotDuplexTransport::remove(name)` (unlink after both sides closed)

## License

GPL-3.0-or-later. See `LICENSE`.
