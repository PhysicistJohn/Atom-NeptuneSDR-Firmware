# Atom-NeptuneSDR-Firmware

Firmware is the firmware half of the NeptuneSDR/HAMGEEK P210 development
environment. It owns immutable firmware inputs, hostile-input parsers,
P210/Xilinx handoff validation, the ARM guest FFT streamer, the board-side FFT
ABI, and assembly of a QEMU-development runtime. The digital twin lives in a
separate repository and consumes the machine-readable interface and emitted
runtime manifest.

The exact repository identity is `Atom-NeptuneSDR-Firmware`, published at
<https://github.com/PhysicistJohn/Atom-NeptuneSDR-Firmware>.

## What this produces

The full bundle combines the locked public P210 kernel/device tree with the
official PlutoSDR v0.39 initramfs, builds a static Cortex-A9 FFT/NSFT guest,
and emits a content-addressed manifest. It is explicitly:

- profile `qemu-development`;
- `flashable: false`;
- an ABI-compatible experimental composition, not HAMGEEK factory firmware;
- executable under the matching P210 QEMU twin, without claiming physical RF,
  FPGA timing, USB gadget, power, or flash/recovery equivalence.

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
neptune-firmware validate-locks
neptune-firmware interface
neptune-firmware source-identity
scripts/check.sh
```

`scripts/check.sh` is the no-network source gate: byte-compilation, a
`-Werror` syntax check of the guest C, 24 unit tests, a parse check of every
shell script, and the JSON CLIs. It is the cheap proof that a clone works, and
it finishes in roughly 15 seconds.

None of those commands contacts or flashes a board. `fetch` writes only to a
host content-addressed cache:

```sh
neptune-firmware fetch --cache-dir .cache/firmware
python3 scripts/test_firmware.py --fetch --json
```

## Build the QEMU-development bundle

The ARM guest is pinned to Zig 0.14.1, and the build refuses any other version.
`scripts/build_guest_fft.sh` looks for it at `.cache/fft-toolchain/bin/zig`
unless `ZIG` is set. That path is gitignored, so a fresh clone must supply the
toolchain first. Either install the pinned compiler into the default prefix:

```sh
mamba create -y -p .cache/fft-toolchain zig=0.14.1
```

or unpack the upstream tarball for your platform and point `ZIG` at it:

```sh
curl -LO https://ziglang.org/download/0.14.1/zig-aarch64-macos-0.14.1.tar.xz
tar xf zig-aarch64-macos-0.14.1.tar.xz
ZIG=$PWD/zig-aarch64-macos-0.14.1/zig scripts/build_bundle.sh
```

Substitute `zig-x86_64-macos-0.14.1` or `zig-x86_64-linux-0.14.1` as needed. If
a previous run already created `.cache/fft-toolchain`, it holds a relocation
stamp, and `mamba` will refuse to populate a non-empty prefix; remove that
directory first or use the tarball route.

Optional locations are controlled by `FIRMWARE_CACHE_DIR`,
`FIRMWARE_RUNTIME_OUTPUT`, and `P210_GUEST_OUTPUT`.
`NEPTUNE_FIRMWARE_INTERFACE` overrides the path to the canonical interface
JSON, and `QEMU_SYSTEM_ARM` selects the `qemu-system-arm` executable used by
`qemu_boot.py` when it is not on `PATH`. The script builds the guest,
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

## Ownership boundary

Firmware owns guest firmware source, firmware inputs/locks, extraction and ABI
audit, the canonical board-side interface, and firmware-specific tests. The
separate twin owns behavioral radio/SoC models, QEMU device models, USB/network
emulation, host-side protocol clients, and end-to-end acceptance orchestration.
The repos couple only through the versioned JSON interface and the generated
runtime manifest; neither imports the other's Python implementation.

## Safety and licensing

There is deliberately no flash command. Do not write these derived artifacts
to hardware. Preserve the factory media and obtain a revision-matched recovery
image and vendor procedure first. Source in this repository is MIT licensed;
downloaded inputs retain their upstream licenses. See `NOTICE.md`.

## Operator golden arithmetic

The v2 operator's fixed-point datapath is pinned by
[`specs/golden-arithmetic-v1.md`](specs/golden-arithmetic-v1.md) -- one integer
arithmetic (committed 18-bit twiddle ROM, round-half-to-even everywhere)
reproduced bit-for-bit by the Python reference (Atom-Neural-RL), the C twin core,
the RTL, and the QEMU operator device. See
[`Atom-NeptuneSDR-Twin/cosim/REPRODUCE.md`](https://github.com/PhysicistJohn/Atom-NeptuneSDR-Twin/blob/main/cosim/REPRODUCE.md)
to run every leg.
