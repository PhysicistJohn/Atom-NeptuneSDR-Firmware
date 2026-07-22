# Neptune Edge custom firmware architecture

This document is the implementation contract for branch `hardware/custom-v0`.
It extends, but does not mutate, the repository's QEMU-development profile.
The physical profile targets a HamGeek Neptune/P210 built around an AD9361 and
XC7Z020. Hardware facts that differ across the public HDF, XSA, DTB, and vendor
descriptions remain release blockers until they are measured on one identified
board revision; see `HARDWARE_EVIDENCE.md`.

## Fixed operating points

- The AD9361/programmable-logic ingress domain is 61.44 MSPS.
- The free-running 64-bit timestamp advances in that ingress domain.
- The primary continuous Ethernet product is one channel of S8 or S8BF at an
  exact 55 MSPS.
- The conversion is a filtered rational resampler with ratio 1375/1536,
  implemented as 125/128 followed by 11/12. It is not sample deletion.
- Native-rate S16/S12 products and dual-channel full-rate products use the raw
  tap and triggered DDR capture unless a lower product rate is selected.
- USB is the independent control/recovery plane. UDP over GEM0 Gigabit Ethernet
  is the continuous data plane.
- TX is disabled at reset, absent from the first physical top level, and
  protected by persistent inhibit and explicit disarm contacts.

At MTU 9000, one 55 MSPS S8BF channel consumes 880 Mb/s of IQ payload and
approximately 914.19 Mb/s on the wire while reserving full Edge metadata plus
one discontinuity extension. That is the
baseline, not a claim that an unvalidated board already sustains it. MTU 1500,
dual-channel 55 MSPS S8, and continuous native S16 do not fit one Gigabit link.
`tools/benchmark/transport_budget.py` is the executable budget model.

## Versioned seams

The design has three canonical machine-readable contracts and one generated
profile that binds them:

1. `neptune-edge-data-v1.json`: fixed 64-byte NEDP header, typed extensions,
   formats, revisions, exact rate state, sequence and discontinuity semantics.
2. `neptune-edge-control-v1.json`: fixed 40-byte NECP header, strict typed
   items, CRC-32C, compare-and-commit, and sample-timestamped activation.
3. `neptune-pl-registers-v1.json`: one 4 KiB little-endian AXI-Lite map shared
   by RTL, the PS driver, and generated C/SystemVerilog bindings.
4. `neptune_edge_profile_v1.json`: the custom-v0 execution/rate/safety profile
   plus byte lengths and SHA-256 identities for every canonical seam.

Generators emit the C, Python, SystemVerilog, and golden-vector derivatives.
The source gate rejects hand-edited or stale generated files. The paired Twin
branch locks hashes of these contracts instead of importing Firmwave Python.

## Receive data path

```text
AD9361 RX clock/contact
  -> deterministic dual-channel ingress and 64-bit sample counter
  -> immutable raw tap
  -> DC correction (bypassable)
  -> widely-linear IQ/channel correction (bypassable)
  -> frequency-EQ integration contact and coefficient banks (bypassed until a routed FIR exists)
  -> validity/discontinuity tracking
  -> exact filtered 61.44-to-55 MSPS conversion
  -> fixed or block-floating quantization
  -> product metadata/header construction
  -> DMA completion ring
  -> PS batched UDP service
```

RTL under `hdl/rtl/core` implements the counter/ingress, correction arithmetic,
atomic configuration and coefficient swaps, DDC/NCO/CIC primitives, S16/S12P/
S8 packing, nearest-even S8 and S8BF, block statistics, windows/STFT scheduling,
peak and energy detectors, dual-channel accumulation, trigger control, NEDP
CRC/header construction, and TX safety. Each high-rate block has an explicit
bypass or raw contact where applicable. A non-backpressurable sample clock can
never be disguised as AXI flow control: overflow raises a sticky fault,
increments the discontinuity revision, and marks the affected packet.

The portable source tree does not yet contain a complete FFT transform core,
the frequency-EQ FIR data path, a DDR circular-buffer writer, or a board-routed
AD9361 top level. Window/STFT scheduling, coefficient-bank control, trigger
control, and the corresponding wire layouts are integration contacts, not a
claim that those products already run on the physical FPGA.

The Xilinx FIR implementation is deliberately behind a narrow black-box
wrapper. Coefficients are generated deterministically and audited by response
metrics and hashes. A Vivado IP instance and timing-closed board top level may
only replace that contact after the board interface/clocks are locked.

## Product and memory policy

The normal path uses small elastic FIFOs and a DMA ring. DDR is not inserted
between DSP stages. DDR is reserved for DMA ownership transfer, circular raw
capture, pre/post-trigger retention, coefficient staging when BRAM is
insufficient, and burst download.

Every DMA completion binds the slot generation to sample timestamp, sequence,
configuration/calibration/device/discontinuity revisions, format, quantization
statistics, and exact resampler state. A stale generation or overwritten slot
is an error, not a packet containing ambiguous data. Kernel and userspace use
`ps/include/neptune_stream_uapi.h`; the driver refuses a PL magic/ABI mismatch.

## Processing-system services

The control daemon parses incremental NECP frames with bounded lengths,
CRC-32C and strict item ordering. State changes are validated in shadow state,
compared with expected revisions, committed atomically, and returned with the
same activation timestamp later emitted as a state-change event. The stream
kernel driver is the sole owner of PL MMIO and exposes a separate read-only
status endpoint for control-plane identity/health; no UIO node may map the same
resource. Physical state-changing operations remain fail-closed until the
matched AD9361 and PL atomic-commit path exists. The mock backend drives
conformance tests. The intended USB production transport has a FunctionFS bulk
adapter, with a CDC console reserved for recovery; configfs gadget composition
and live endpoint validation are still board-integration work.

The data service owns the DMA ring and UDP socket, but never changes RF state.
It batches completions, uses jumbo-datagram profiles that fit IPv4/UDP limits,
and converts every detected ring/network loss into counters and Edge
discontinuity semantics. The host capture utility validates header/payload CRC,
sequence, timestamp and resampler continuity while writing replayable records.

## Persistent state and updates

Calibration bundles are device-serial-bound, SHA-256 checked, coefficient-
bounded and revision-chained. Installation and rollback are atomic filesystem
operations. The protocol records a requested ingress activation timestamp and
the portable PL swaps shadow banks at a block boundary. Enforcing that exact
requested tick through the board-routed coefficient path remains a physical
integration gate; data packets must name the revision actually active.

Updates use signed canonical manifests, Ed25519 verification, per-artifact
size/SHA-256 checks, strictly increasing rollback indexes, and removable-SD A/B
slots. The updater must stage only the inactive slot. QSPI writes are forbidden
by both board policy and the manifest verifier. See `CALIBRATION.md` and
`RECOVERY.md`.

## Build and release gates

Portable CI runs Python/C source tests, golden vectors, Icarus RTL compile and
self-checking simulations, generator freshness, hostile-input tests, packaging
checks, and shell syntax checks. A physical release additionally requires all
of the following artifacts from the pinned x86_64 Ubuntu 22.04 Vivado 2023.2
worker:

- exact board constraints, ADI/Xilinx IP configuration and generated sources;
- synthesis utilization, implementation timing, CDC, DRC and bitstream reports;
- FSBL, bitstream, U-Boot, kernel, DTB and rootfs hashes in a signed manifest;
- hardware loopback results for alignment, phase, timestamps and calibration;
- sustained DMA, jumbo-UDP, CPU-load, packet-loss and USB-under-load results;
- power-cycle, watchdog, A/B rollback, recovery and overnight soak results.

No report may be replaced by a simulated assertion. The current source tree is
a tested implementation scaffold and protocol-compatible digital proof path;
it is not yet a flashable physical-board release because the matched hardware
handoff, AMD installer/EULA, and connected board are external inputs.
