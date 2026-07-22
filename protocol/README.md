# Neptune Edge v1 wire contracts

The canonical schemas are
[`neptune-edge-data-v1.json`](../specs/neptune-edge-data-v1.json) and
[`neptune-edge-control-v1.json`](../specs/neptune-edge-control-v1.json). The
files in `generated/` and `golden/` are deterministic derivatives; edit the
schemas and generator, never the derivatives alone.

This contract is additive. It does not replace or modify
`p210-firmware-interface-v1.json`, the P210 FFT MMIO ABI, or NSFT-v1. A custom
firmware image may expose NSFT-v1 as a compatibility product while using
Neptune Edge v1 for its primary data and control planes.

## Data-plane invariants

Every UDP payload begins with a fixed 64-byte `NEDP` header followed by
zero or more typed, four-byte-aligned extensions and then the product payload.
The header names the stream, per-stream sequence number, 64-bit sample
timestamp, active configuration/calibration/device-state revisions, and the
current discontinuity revision. CRC-32C covers the complete header. A typed
extension can additionally carry a payload CRC-32C.

`sample_timestamp` is always a tick of the free-running 61.44 MHz ingress
sample counter. It is not wall-clock time and it does not change units for a
derived product. The continuous egress conversion is exactly:

```text
61,440,000 * 1,375 / 1,536 = 55,000,000 samples/second
```

For a 55 MHz packet, `RESAMPLER_STATE` is mandatory. Given packet-local output
sample index `k`, its associated ingress tick is:

```text
input_timestamp + floor((phase_numerator + k * 1536) / 1375)
```

After `sample_count` output samples, both ends advance the state with:

```text
total = phase_numerator + sample_count * 1536
input_timestamp += floor(total / 1375)
phase_numerator = total mod 1375
```

This avoids floating-point timestamps and preserves exact continuity across
packet boundaries.

S16, S12P, S8, and S8BF have fixed byte layouts in the data schema. S8BF
requires `QUANTIZATION`; an original ADC-domain estimate is `q * 2^exponent`.
RMS and peak use unsigned Q16.16 ADC-code units, clipping counts saturated I/Q
components, and valid/invalid counts measure complex channel samples. The RF
state carries independent RX1/RX2 analog gain and gain mode, digital gain,
temperature, and lock state, while the base header binds the calibration
revision. Raw streams use calibration revision zero and bypass every correction
stage.

Derived products are no longer implied by prose. FFT, STFT, detector events,
triggered-capture segments, validity bitsets, status snapshots, and dual-channel
products each have a fixed typed extension. The generated decoder checks their
payload geometry, encoding width, enabled-channel count, timestamp convention,
and required metadata. A validity bitset is a distinct `VALIDITY_MASK` packet,
so appending it can never make an IQ payload length ambiguous.

A continuity break increments `discontinuity_revision` once, sets the
`DISCONTINUITY` flag on the first affected packet, and emits a typed
`DISCONTINUITY` packet or extension. Samples are never silently removed or
interpolated. State changes likewise carry old/new revisions and an exact
activation timestamp; the first affected packet carries the new revisions.

## Control-plane invariants

USB control messages use a fixed 40-byte `NECP` header and typed payload items.
The header and payload have independent CRC-32C values. State-changing requests
use a nonzero transaction ID, compare expected revisions, validate atomically,
and activate on a stated 61.44 MHz sample boundary. A successful response and
the asynchronous state-change event report the same activation timestamp and
resulting revisions.

The control schema maps every command code to canonical typed items. Fixed
layouts cover identity, health, RF/pipeline/DDC/FFT/STFT/normalization/detector
configuration, streams, trigger capture, persistent calibration, sticky
counters, TX safety, recovery, logs, and signed A/B update transfer. Chunk
items are bounded to 256 bytes and require length, range, zero-padding, and
CRC-32C validation. A target may respond `UNSUPPORTED`; it may not acknowledge
a state change that its board backend did not atomically apply.

`generated/neptune_edge_profile_v1.json` hashes the schemas, register map,
bindings, golden vectors, board profile, calibration/update contracts, and
SystemVerilog package into the single `p210-edge-custom-v0` Twin seam.

Run `python3 scripts/generate_protocol.py` after an intentional schema change.
CI and the source gate run the non-mutating equivalent:

```sh
python3 scripts/generate_protocol.py --check
```

The golden-vector JSON contains complete wire messages as hexadecimal plus
SHA-256 digests. It is suitable for HDL, firmware, host SDK, capture/replay,
and digital-twin conformance tests.
