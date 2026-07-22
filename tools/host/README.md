# Neptune host tools

These Python tools use the generated Neptune Edge v1 binding as their wire
source of truth.

- `neptune_ctl.py` sends strict control requests over a daemon Unix socket or
  an already-bound raw full-duplex USB bulk/relay endpoint. TX enable is not
  exposed.
- `neptune_rx.py` validates UDP headers, extensions, header/payload CRCs,
  stream sequences, ingress timestamps, and rational 61.44-to-55 MHz phase.
  Optional captures use a simple little-endian `u32 length + datagram` format.

Example against the mock daemon:

```sh
ps/control-daemon/build/neptune-control-daemon --mock --unix /tmp/neptune.sock
python3 tools/host/neptune_ctl.py --unix /tmp/neptune.sock identity
python3 tools/host/neptune_rx.py --port 50000 --count 1000
```

Build and integration tests:

```sh
make -C tools/host clean test
```

The raw-device transport is not a bundled libusb gadget manager: the board
image must configure FunctionFS/configfs and expose an appropriate endpoint.
The receiver is a correctness/diagnostic tool, not a proven line-rate capture
engine; production hosts will need tuned socket buffers, CPU affinity, and
platform-specific receive batching.
