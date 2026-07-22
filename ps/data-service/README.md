# Neptune data service

This service moves completed PL DMA-ring blocks to canonical Neptune Edge Data
Plane v1 UDP datagrams. The high-rate path uses only the dedicated
`/dev/neptune-stream` UAPI; it never performs userspace libiio sample reads.
The mapping is read-only, ACQUIRE/RELEASE uses exact slot-generation tokens,
UDP sends are batched, and packet payloads remain two-iovec references to DMA
memory until transmission completes.

## Metadata contract

Every packet is built from an immutable RF/device-state snapshot keyed by
`device_state_revision` and its ingress-sample activation timestamp. The block
revision, configuration revision, and timestamp must resolve to the exact
active snapshot. Missing, early, stale, or superseded snapshots stop the
service with `ESTALE`; `METADATA_COMPLETE` is never asserted from guessed
state. Additional snapshots can be appended in increasing activation/revision
order with `nds_service_add_rf_snapshot()` before their first affected block.

The command-line service currently accepts one explicit startup snapshot. A
future authenticated control-to-data IPC must deliver later snapshots before
their activation timestamp. Until that IPC exists, a live RF/device-state
transition correctly stops the process instead of mislabeling samples.

Discontinuity sequence and timestamps are tracked in the 61.44 MHz ingress
domain. `lost_input_samples` is derived from exact timestamps and rational
resampler phase; unknowable ranges are encoded as `UINT32_MAX`. A discontinuity
already marked and revisioned by the kernel is not incremented a second time.

## Capacity and formats

The v1 packet prefix maximum is 272 bytes: base header, RF, quantization,
resampler, discontinuity, block statistics, and payload CRC. The builder
precomputes the required prefix and checks capacity before every append. It
supports the initial RAW S16/S12P, CALIBRATED S16/S12P when advertised by PL,
and NORMALIZED S8/S8BF products. Exact Ethernet framing overhead is included in
the 1 Gb/s admission check. Full-rate raw streaming is therefore rejected;
single-channel 55 MS/s S8 requires the jumbo-MTU profile.

Example (development loopback explicitly enabled):

```sh
ps/data-service/build/neptune-data-service \
  --destination 127.0.0.1 --port 50000 --allow-loopback \
  --center-frequency 915000000 --rf-bandwidth 20000000 \
  --configuration-revision 0 --device-state-revision 0 \
  --state-activation-timestamp 0 \
  --rx1-gain-mdb 30000 --rx1-gain-mode manual \
  --digital-gain-q16-16 65536 --temperature-mc 42000 \
  --pll-lock-mask 1 --device-flags 0
```

## Build and test

```sh
make -C ps/data-service clean test
```

The target runs normal and ASan/UBSan suites, every valid extension
combination (including the 272-byte maximum), DMA ownership/error cleanup,
exact-once discontinuities, ingress-tick loss accounting, timestamped
gain/AGC/temperature/PLL transitions, and a C-to-generated-Python wire fixture.
The real Linux ioctl/mmap branch also compiles cleanly and runs this suite on
Ubuntu 22.04; exercising the ioctls against a P210 still requires hardware.

Not implemented here: FFT/STFT/detector products, live snapshot IPC, triggered
DDR capture, kernel-bypass networking, or physical throughput/coherence
evidence.
