# Neptune PL stream driver foundation

`neptune_stream` is the proposed Linux owner for the continuous PL metadata and
payload path. It allocates one DMA-coherent receive ring, submits slots through
the DT-provided DMAEngine receive channel, validates DMA callbacks, and exposes
completed slots to one userspace data service through `poll`, fixed-width
ioctls, and a read-only `mmap`.

This is source-level architecture, not a live-kernel or hardware-validation
claim. No released bitstream in this repository implements the whole DMA slot
contract. Probe therefore requires the canonical generated `NEPT` PL identity
and ABI, raw-bypass capability, a DMAEngine DEV_TO_MEM channel, a 32-bit DMA
mask, mmap-capable coherent DMA, a PL fault IRQ, and a dedicated reset. An
unknown or partial endpoint does not bind.

## Compatibility boundary

The compile-time support window is:

```text
Linux 5.10 <= supported source API < Linux 6.13
Zynq little-endian, 4 KiB pages, 32-bit DMA addresses
```

The window covers the API shape audited for this foundation; it is not a claim
that every intermediate vendor tree builds unchanged. Kernel 4.14, including
the repository's pinned QEMU-development image, is deliberately rejected by a
preprocessor error. Before widening either boundary, compile against the exact
locked kernel configuration, run sparse and Coccinelle, boot the matching DT,
exercise DMA API debugging, and complete the hardware tests below.

The driver uses Linux's coherent DMA allocation and mmap APIs rather than
physical-address mappings. See the upstream
[DMA API documentation](https://docs.kernel.org/6.5/core-api/dma-api.html).
The high-rate path has no file `read` or `write` operation and does not route
payloads through userspace libiio reads. Control-plane AD9361 use may remain in
its own driver and service; it does not own these payload slots.

## Ownership and lifecycle

There is one open file and one userspace owner. Each slot follows only these
transitions:

```text
FREE --DMAEngine submit--> DMA --validated callback--> READY --ACQUIRE--> USER
  ^                                                           |
  +---------------- STOP or stopped RELEASE ------------------+
                    DMA <-- running RELEASE -------------------+
```

Linux submits only `FREE` slots as fixed-length DMAEngine descriptors. A DMA
callback is accepted only for a `DMA` slot, and a released generation token is
the only path back to `FREE`. The stream may stall when no descriptor is
available but must report any loss at the non-backpressurable RF ingress; it
must never overwrite `READY` or `USER` slots.

Normal userspace order is:

1. Open `/dev/neptune-stream` read-only. A second open returns `EBUSY`.
2. `GET_ABI`; reject an unknown major version or missing feature.
3. Map exactly `mmap_length` bytes at offset zero using `MAP_SHARED` and
   `PROT_READ`. Partial, writable, private, executable, or nonzero-offset maps
   fail.
4. `CONFIGURE` a stream ID, channel mask, Edge-v1 packet type and sample
   format, and maximum complex samples per channel per slot.
5. `START`.
6. Wait with `poll`/`epoll`, then call `ACQUIRE` repeatedly until `EAGAIN`.
7. Read the payload at
   `base + slot_index * slot_bytes + payload_offset`. The ioctl metadata is the
   authoritative completion snapshot.
8. `RELEASE` the exact slot index and generation. Stale, duplicate, or foreign
   releases fail with `ESTALE`.
9. Release all owned slots before `STOP`; otherwise `STOP` returns `EBUSY`.

`/dev/neptune-pl-status` is a separate, nonexclusive, read-only view from the
same kernel/MMIO owner. It accepts only `GET_ABI` and `GET_PL_STATUS`, allowing
the control/health service to inspect canonical PL identity, build,
capabilities, coherent sample counter/epoch, revisions, stream/DMA diagnostics,
and TX safety while `/dev/neptune-stream` is exclusively owned. Every
state-changing ioctl fails with `ENOTTY` on the status node. `GET_ABI` describes
the shared stream ABI and ring endpoint, so its `EXCLUSIVE_OPEN` feature applies
to `/dev/neptune-stream`, not to the status node.

`ACQUIRE` is intentionally nonblocking so one readiness notification can drain
a batch without one ioctl sleeping inside the driver. Closing the final file
forces quiescence, invalidates generations, and returns the device to `IDLE`.
Probe establishes the dedicated reset baseline. A normal `START` revalidates
identity/status but deliberately does not reset PL, because doing so would erase
sample-timestamped pipeline and calibration state; hard reset is recovery only.

## Slot and format contract

The shared header is `ps/include/neptune_stream_uapi.h`. Every slot begins with
the 128-byte little-endian `neptune_stream_block_header`; payload immediately
follows. The kernel verifies magic, header size, known flags, stream identity,
channel mask, format, sample count, and exact packed payload length before a
slot becomes visible.

ABI v1 deliberately uses the Edge-v1 numeric format identity:

| Value | Format | Payload bits per complex sample per enabled channel |
|---:|---|---:|
| 1 | S16 | 32 |
| 2 | S12P | 24 |
| 3 | S8 | 16 |
| 4 | S8BF | 16 |

Value 5 is reserved and rejected. `sample_count` is the number of complex time
samples for each enabled channel. For two enabled channels, payload size is
therefore twice the single-channel value.

The UAPI also reserves the full Edge-v1 packet-type number space. This initial
driver exposes only contracts provable from canonical PL capability bits:
`RAW_IQ=1` with S16/S12P, `CALIBRATED_IQ=2` with S16/S12P only when canonical
`CALIBRATED_IQ` capability bit 12 is present, and `NORMALIZED_IQ=3` with S8/S8BF
when the matching resampler/quantizer capability exists. FFT, STFT, events,
capture, status, state-change, and discontinuity packets remain explicit but
unsupported. The service must never infer product identity from sample format.

Every IQ completion carries a 64-bit epoch-local `output_sample_index`; it must
advance by `sample_count`, including for native RAW and CALIBRATED products, and
need not equal the absolute ingress sample timestamp. It resets only with an
explicitly marked new continuity epoch. Normalized completions also carry
`resampler_phase_numerator` in 0..1374. For `n` 55 MHz output samples the driver
checks the next block against `total = phase + n * 1536`, advancing the 61.44
MHz ingress timestamp by `total / 1375` and phase by `total % 1375`. Native RAW
and CALIBRATED blocks require phase zero and advance timestamp by `n`.

S8BF v1 is a fixed PL policy: deterministic peak scaling, one headroom bit,
round-to-nearest-even, and reconstruction `ADC_code = q * 2^exponent`, with
exponent restricted to -31..31. `block_rms_q16` and `block_peak_q16` are
unsigned Q16.16 pre-quantization ADC-code magnitudes and clipping counts scalar
I/Q saturations.

The mmap is zero-copy for payload bytes: userspace reads the allocation the PL
wrote. `ACQUIRE` copies only a fixed 128-byte metadata snapshot so userspace
cannot race a recycled header and never needs to trust mutable ownership data
inside the mapping.

## Fault visibility

Sequence gaps, overflow, malformed metadata, restarts, DMA faults, FIFO faults,
and interface faults increment persistent counters. The next valid completion
receives a discontinuity flag and monotonically increasing discontinuity
revision. DMA and interface faults stop the endpoint in `ERROR`; an observable
FIFO overflow marks discontinuity and may continue. Poll reports `EPOLLERR` for
fatal state, `EPOLLPRI` for pending nonfatal discontinuity metadata, and
`EPOLLIN` for ready slots.

The 32-bit discontinuity revision never wraps or reuses a value. An event that
would advance it past `UINT32_MAX` puts the endpoint in a poisoned `ERROR` state
and stops streaming; recovery requires reprobe/reboot into a fresh documented
continuity epoch.

`dropped_blocks` counts blocks proven lost by source-sequence gaps, rejected
malformed completions, and READY/USER blocks abandoned by stop or process
death. `overrun_events` is separate because the hardware status bit does not
provide an exact loss count. Counters reset only while stopped and with no
userspace-owned slot.

## PL and device-tree integration

MMIO constants and bitfields come only from the generated
`ps/include/neptune_pl_registers_v1.h`, whose source is
`specs/neptune-pl-registers-v1.json`. `neptune_stream_regs.h` adds no addresses.
In summary, integration must provide:

- exact `NEPT` hardware identity and ABI 1.0;
- canonical raw, DMA-slot-header-v1, optional calibrated/resampler/quantizer
  capability bits, and stream registers;
- a DMAEngine `rx` slave channel accepting one descriptor per block;
- a fault IRQ backed by canonical W1C global-fault bits;
- a stream disable that becomes inactive, plus a dedicated hard reset.

The binding is
`Documentation/devicetree/bindings/misc/hamgeek,neptune-stream-dma-v1.yaml`.
`ring-slots`, `slot-bytes`, `dmas`, and `dma-names = "rx"` are required; there
are no guessed defaults. Any reserved-memory policy belongs to the selected DMA
controller and must be validated with that controller's binding.

`hamgeek` is a repository-local experimental vendor prefix. Before submitting
this binding upstream or treating `dt_binding_check` as an upstream-clean gate,
register the agreed legal vendor name and prefix in the target kernel tree's
`vendor-prefixes.yaml`; do not silently substitute another vendor identity.

This endpoint is static SoC hardware. Driver sysfs bind/unbind attributes are
suppressed, and runtime DT-overlay removal while open is outside the v1
lifetime contract.

The platform driver is the sole MMIO owner for its exact 4 KiB canonical PL
resource. `/dev/neptune-pl-status` is a second API surface on that same owner,
not another mapping. A UIO node over the same physical range must not coexist.
State-changing physical controls remain unimplemented/fail-closed until narrow
kernel ioctls or an MFD/regmap parent are specified and reviewed.

## Build and validation

Add this directory to the selected kernel's parent Kconfig and Makefile, copy
or install `ps/include/neptune_stream_uapi.h` as a userspace-visible UAPI
header, make `ps/include/neptune_pl_registers_v1.h` available to kernel and
control-service builds, and enable `CONFIG_NEPTUNE_STREAM`.

For an out-of-tree compile against an exact kernel header tree:

```sh
make -C ps/kernel/neptune_stream KDIR=/path/to/linux-build
```

Host-static checks are intentionally separate from kernel validation:

```sh
python3 ps/kernel/neptune_stream/tests/test_neptune_stream.py
```

The current source was also compiled out of tree with `W=1` against Ubuntu's
Linux `5.15.0-185-generic` headers. That proves one supported API point builds;
the module was not loaded and this is not DMA or hardware validation.

CI repeats this as a job explicitly named **Generic Linux API compile
(non-release)** against the runner's generic headers. It does not select or
validate the target Zynq kernel, configuration, ARM compiler, or final board
device tree. A strict source-only `dt_binding_check` is intentionally not
claimed here: the repository does not yet lock the target kernel schema tree,
and its experimental `hamgeek` prefix must first be registered under the agreed
legal vendor identity in that tree. YAML parsing or validation against an
unrelated runner kernel would not be an acceptable substitute.

Before hardware acceptance, additionally require:

- exact target-kernel/config/toolchain `W=1` module and built-in compilation,
  plus sparse output;
- target-tree `dt_binding_check` after vendor-prefix registration;
- final board DTS/DTB hashes and a clean target-tree `dtbs_check` report;
- live module load and device bind/probe logs on the Zynq target;
- DMA API debugging and IOMMU/CMA or reserved-memory coverage;
- IRQ loss, malformed completion, stale release, and ring-exhaustion tests;
- mmap lifetime tests across fork, close, process death, and service restart;
- sustained line-rate capture with explicit sequence/discontinuity accounting.
