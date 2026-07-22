# Atom-NeptuneSDR_Firmwave

Firmwave is the firmware half of the NeptuneSDR/HAMGEEK P210 development
environment. Branch `hardware/custom-v0` adds a ground-up Neptune Edge FPGA,
control/data-plane, kernel, build, calibration, update, and verification stack
alongside the original immutable QEMU-development profile. The digital twin
lives in the parent/sibling repository and consumes only versioned, hash-locked
wire contracts.

The exact repository identity is `Atom-NeptuneSDR_Firmwave`, published at
<https://github.com/PhysicistJohn/Atom-NeptuneSDR_Firmwave>.

## Profiles and outputs

The legacy QEMU bundle combines the locked public P210 kernel/device tree with the
official PlutoSDR v0.39 initramfs, builds a static Cortex-A9 FFT/NSFT guest,
and emits a content-addressed manifest. It is explicitly:

- profile `qemu-development`;
- `flashable: false`;
- an ABI-compatible experimental composition, not HAMGEEK factory firmware;
- executable under the matching P210 QEMU twin, without claiming physical RF,
  FPGA timing, USB gadget, power, or flash/recovery equivalence.

The custom branch additionally produces portable RTL simulations, generated
C/Python/SystemVerilog protocol bindings, a strict USB NECP control daemon, a
DMA-ring/UDP data service and kernel ABI, calibration/update admission logic,
host/Twin conformance vectors, and a fail-closed physical build definition. It
does **not** claim a physical bitstream or boot image until one matched board
handoff is reconciled and Vivado implementation/hardware reports pass. The
[architecture contract](docs/NEPTUNE_EDGE_ARCHITECTURE.md) records that boundary.

The canonical seam is
[`specs/p210-firmware-interface-v1.json`](specs/p210-firmware-interface-v1.json).
It fixes the 61.44 MSPS / 50 MHz, two-channel IQ16 contact, the proposed
65,536-point FFT MMIO/DMA contract, IIOD TCP port 30431, and NSFT-v1 TCP port
30432. A byte remains eight bits: DDR's “16-bit” specification is bus width,
while each I or Q sample occupies a signed 16-bit transport slot with 12
significant AD9361 bits.

## Install and inspect

Python 3.9 or newer is sufficient for artifact inspection:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -e .
neptune-firmwave validate-locks
neptune-firmwave interface
neptune-firmwave source-identity
scripts/check.sh
```

The gate uses Icarus Verilog when available. It compiles the portable receive
pipeline and runs the self-checking SystemVerilog benches in addition to the C
and Python suites. The pinned Vivado worker is managed separately:

```sh
tools/vivado/vm.sh status
tools/vivado/vm.sh shell
```

It is an Ubuntu 22.04 x86_64 VM because Vivado 2023.2 is not a native Apple
Silicon tool. Installing AMD's suite requires the authenticated installer and
explicit EULA acceptance; `tools/vivado/install-vivado-2023.2.sh` verifies both
before it writes `/opt/amd` in the guest.

CI's **Generic Linux API compile (non-release)** job is only an out-of-tree
source/API smoke test against the GitHub runner's current generic Linux headers.
It is not the target Zynq kernel build, a device-tree schema gate, a final DTB
check, sparse analysis, or a live module-load test. Physical release still
requires versioned evidence from one locked target kernel tree, configuration,
ARM toolchain, reconciled board DTS, and routed bitstream: exact-kernel `W=1`
and sparse logs, target-tree `dt_binding_check`, a final DTS/DTB hash plus
`dtbs_check`, and successful module load/probe with DMA, IRQ, and mmap tests on
the board. The repository intentionally does not substitute a generic runner
kernel or a YAML syntax parser for those artifacts.

None of those commands contacts or flashes a board. `fetch` writes only to a
host content-addressed cache:

```sh
neptune-firmwave fetch --cache-dir .cache/firmware
python3 scripts/test_firmware.py --fetch --json
```

Before treating any public artifact as a physical-board handoff, run the
fail-closed hardware-evidence gate:

```sh
neptune-firmwave validate-hardware-manifest --json
```

The checked-in candidate intentionally returns status `1`: the locked SD HDF,
separately locked XSA, and public DTB disagree about DDR, AD9361 interface and
channel topology, GPIO capacity, and build identity. See the
[hardware-evidence gate](docs/HARDWARE_EVIDENCE.md) before replacing that
candidate with revision-matched observations.

## Build the QEMU-development bundle

The ARM guest is pinned to Zig 0.14.1. Point `ZIG` at that executable and run:

```sh
ZIG=/path/to/zig-0.14.1 scripts/build_bundle.sh
```

Optional locations are controlled by `FIRMWAVE_CACHE_DIR`,
`FIRMWAVE_RUNTIME_OUTPUT`, and `P210_GUEST_OUTPUT`. The script builds the guest,
fetches and verifies every locked input, validates both boot environments and
the XSA, then prepares the FFT runtime and `runtime-manifest.json`.

For individual stages, use:

```sh
scripts/build_guest_fft.sh
scripts/fetch_firmware.py --json
scripts/test_firmware.py --json
scripts/prepare_runtime.py --fft-streamer .cache/p210-guest/neptune-fft-streamer --json
scripts/qemu_boot.py --artifact p210-sd-boot --json
```

`qemu_boot.py` is dry-run unless `--run` is supplied and never attaches host
USB or block devices. See [the FFT ABI](docs/P210_FFT_ABI.md),
[provenance boundary](docs/PROVENANCE.md), and the
[vendor-material request](scripts/request_vendor_materials.md).

## Physical-board scaffold

Branch `hardware/custom-v0` also carries a fail-closed physical-board build
contract under `board/p210`. It preserves the QEMU profile and does not claim a
bootable or flashable custom firmware image. The checked-in definition records
one matched compatibility ID, an SD-only/TX-disabled safety policy, every
required build output, and the sources and tools that are still unresolved.

Structural validation works without Xilinx tools:

```sh
python3 scripts/board_doctor.py --validate-only
python3 scripts/build_board.py --plan
```

The real doctor and builder refuse to proceed until all sources, proprietary and
open toolchains, and direct-argv stages are immutably locked and locally
matched. The builder emits regular files only; it has no block-device writer or
QSPI update path. See [the board-build contract](docs/BOARD_BUILD.md).

## Neptune Edge contracts

The primary continuous product is one channel of S8/S8BF IQ at exactly 55 MSPS
over jumbo-frame Gigabit Ethernet; the PL remains at 61.44 MSPS internally and
the raw tap retains native capacity. The exact filtered rate ratio is
1375/1536. Dual-channel or S16 native-rate products are intended for the DDR
circular-capture path once its board writer is integrated, or can be emitted at
a lower configured rate, because they cannot fit one 1 Gb/s link. Run the
executable budget:

```sh
python3 tools/benchmark/transport_budget.py \
  --sample-rate 55000000 --format S8BF --channels 1 --mtu 9000 --require-fit
```

NEDP data messages have a 64-byte fixed header; NECP USB control messages have
a 40-byte fixed header. Both use typed, bounded extensions/items and CRC-32C.
Sequence, ingress timestamp, configuration, calibration, device-state and
discontinuity revisions are mandatory semantics. See `protocol/README.md`.

Calibration and update inputs can be checked independently:

```sh
neptune-firmwave validate-calibration bundle.json --device-serial SERIAL
neptune-firmwave validate-update manifest.json \
  --artifact-root staged --public-key update-public.pem \
  --accepted-rollback-index 7
```

Update validation permits only signed, rollback-advancing removable-SD A/B
images and explicitly forbids QSPI. It does not write a block device. See
[calibration](docs/CALIBRATION.md) and [recovery](docs/RECOVERY.md).

## Ownership boundary

Firmwave owns guest firmware source, firmware inputs/locks, extraction and ABI
audit, the canonical board-side interface, and firmware-specific tests. The
separate twin owns behavioral radio/SoC models, QEMU device models, USB/network
emulation, host-side protocol clients, and end-to-end acceptance orchestration.
The repos couple only through the versioned JSON interface and the generated
runtime manifest; neither imports the other's Python implementation.

## Safety and licensing

There is deliberately no flash command. Do not write these derived artifacts
to hardware. Preserve the factory media and obtain a revision-matched recovery
image and vendor procedure first. Project-authored userspace, Python, and RTL
are MIT licensed; Linux-facing sources carry their own SPDX alternatives and
license texts under `LICENSES`. Downloaded inputs retain their upstream
licenses. See `NOTICE.md`.
