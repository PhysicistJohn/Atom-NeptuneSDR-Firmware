# Neptune control daemon

This directory contains the transport-independent Neptune Edge Control Plane
(NECP) v1 parser/state machine and its executable transports.

The parser accepts bounded raw NECP frames, validates header and payload
CRC-32C values, item ordering, lengths, required/unknown items, reserved bits,
transaction identifiers, revision preconditions, and atomic activation times
before calling a backend. Configuration revisions boot at zero, advance only
after a backend-confirmed commit, and fail closed rather than wrapping.

## Backends and ownership

`--mock` is deterministic and is used by unit and host-integration tests. The
Linux backend opens `/dev/neptune-pl-status`, the nonexclusive read-only status
node owned by the `neptune_stream` platform driver. It validates the driver ABI
and canonical PL magic/ABI, then obtains coherent sample-counter, build,
health, stream, DMA, and TX-safety state with read-only ioctls.

There is deliberately no UIO or direct MMIO path here. The kernel driver is the
sole owner of the PL register resource. RF, pipeline, stream-lifecycle, and
counter-reset mutations return `UNSUPPORTED` on the physical backend until an
atomic AD9361-plus-PL apply/acknowledgement ABI exists. The daemon must never
report a successful physical change that it cannot prove.

TX is not armed by this implementation. On startup the Linux backend requires
the kernel-owned status path to prove TX is disarmed. A missing persistent
inhibit file means inhibited; an existing file must contain exactly `0\n` or
`1\n`, must not be a symlink, and must have safe production ownership/mode.

## Transports

- `--stdio`: raw concatenated NECP frames for recovery and tests.
- `--unix PATH`: raw full-duplex local stream used by the host CLI.
- `--functionfs PATH`: FunctionFS bulk endpoints for the intended USB control
  plane, plus CDC ACM supplied by the system gadget configuration.

FunctionFS descriptors are supplied by the daemon, but configfs gadget setup,
endpoint permissions, and CDC ACM composition remain board-image integration
work.

## Build and test

```sh
make -C ps/control-daemon clean test
ps/control-daemon/build/neptune-control-daemon --mock --unix /tmp/neptune.sock
```

Tests cover strict framing/CRC behavior, semantic length fuzzing, atomic
revision and timestamp behavior, revision exhaustion, TX inhibit, line-rate
stream validation, the read-only status-node contract, and fail-closed physical
commits. These are source and mock tests, not evidence of validation on a P210.
