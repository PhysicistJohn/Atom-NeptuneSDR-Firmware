# P210 physical-board build scaffold

## Status and boundary

Profile `p210-sd-development-v0` is a structurally complete, fail-closed build
contract for a physical P210. It does not currently build a bootable image. The
checked-in source lock and stage plan name unresolved inputs explicitly, so the
doctor and builder stop before creating an output directory.

The existing `qemu-development` profile is unchanged. Its public P210/Pluto
composition remains non-flashable and is not promoted into this board profile.

The board profile permits only a regular-file image intended for removable SD.
There is no command that opens a block device, writes QSPI, updates U-Boot state,
or enters DFU. Preserve the factory media and establish a revision-matched
recovery procedure before installing any future artifact.

## Definition files

`board/p210` contains four independently hashed build definitions:

- `source-lock.json` binds every source to compatibility ID
  `p210-xc7z020-custom-v0`. A locked Git source needs an HTTPS URL, exact
  40-hex commit, and checkout path. A locked file needs a path, byte count, and
  SHA-256.
- `toolchain-lock.json` records direct version probes. Vivado, XSCT, and Bootgen
  are expected from user-supplied Xilinx 2023.2 installation media; this
  repository neither contains nor downloads them. The ARM cross compiler and
  rootfs generator remain unresolved until matching source selections exist.
- `profile.json` fixes the safety policy and every required output path.
- `build-plan.json` defines dependencies and direct argv commands. Unresolved
  stages carry a reason and no executable command.

The config directory is policy input, not proof that a generated binary obeys
it. In particular, `calibration-defaults.json` contains only uncalibrated
identity/bypass state and makes no measurement claim.

## Doctor and plan

Validate the checked-in structure in CI without needing proprietary tools:

```sh
python3 scripts/board_doctor.py --validate-only --json
```

Probe the real environment and get a complete blocker list:

```sh
python3 scripts/board_doctor.py --json
```

Show the deterministic dependency order without creating files:

```sh
python3 scripts/build_board.py --plan
```

A non-validate-only doctor exits nonzero until every source, artifact-producing
tool, and stage is locked and locally matched. `build_board.py` enforces the
same gate before it creates its output directory.

## Resolving the build

Resolve one source at a time from evidence, not by replacing `unresolved` with a
guess:

1. Establish the PCB revision, connector topology, boot-mode behavior, DDR part,
   Ethernet PHY, USB device wiring, and factory recovery procedure.
2. Lock one complete HDL/constraints/IP source tree. Its routed bitstream,
   hardware export, timing reports, utilization report, and CDC report must be
   products of the same source state and Vivado version.
3. Generate the FSBL BSP from that exact hardware export. Lock a P210 U-Boot
   commit and SD-only environment; do not reuse an unrelated ZedBoard binary.
4. Lock Linux and DTS sources matched to the AD9361/AXI-DMAC HDL. Verify the USB
   controller role from physical wiring rather than changing `dr_mode` on faith.
5. Lock the cross toolchain and minimal rootfs generator. The rootfs must omit
   flash writers, network `iiod`, HTTP, SSH, and mass-storage update services by
   default.
6. Lock the control daemon, canonical protocol schema, and identity calibration
   defaults. Device-specific calibration belongs in separately versioned data.
7. Replace each unresolved stage with a direct argv array. Shell/privilege/
   `env` launchers, inline interpreter code, embedded or direct block-device
   paths, and known flash-writer executables are rejected. Script files remain
   allowed because their locked source is auditable; this check is a guardrail,
   not an operating-system sandbox.
8. Replace `source_date_epoch: 0` with an evidence-backed fixed epoch shared by
   every component build.

Stage commands receive these deterministic contacts:

- working directory: repository root;
- `LC_ALL=C` and `TZ=UTC`;
- `SOURCE_DATE_EPOCH` from the build plan;
- `NEPTUNE_BOARD_DIR` and `NEPTUNE_BOARD_OUTPUT`;
- token expansion for `{repository}`, `{board}`, `{output}`, and
  `{source_date_epoch}`.

Commands run directly without a shell. The orchestrator never cleans or deletes
an output tree and refuses `/` or the user home directory as an output.

## Required outputs and manifest

A successful plan must create all of the following before a manifest is
written:

- matched FSBL, bitstream, U-Boot ELF, and `BOOT.BIN`;
- kernel, device tree, and rootfs;
- the exact control-daemon executable placed in the rootfs;
- calibration defaults and canonical protocol schema placed in the rootfs;
- post-synthesis timing, post-route timing, utilization, and CDC reports;
- one regular-file removable-SD image.

`board-build-manifest.json` records a relative path, role, byte count, and
SHA-256 for every output. It also records hashes of all four definitions, the
Firmwave Git source identity, locked tool-version observations, the safety
policy, and the fixed source epoch. Manifest creation is atomic and fails if an
output is missing, a symlink, or outside the output root.

Even a complete manifest retains:

```text
physical_validation: false
flashable: false
installation.medium: removable-sd
installation.qspi_write_allowed: false
safety.tx_enabled_at_boot: false
```

Those fields are not promoted automatically by a successful build. Hardware
validation, recovery testing, and a separately reviewed installation workflow
must remain distinct gates.

## Minimum physical validation before use

- Boot from removable SD repeatedly with factory QSPI untouched.
- Confirm TX outputs remain disabled through power-up, boot, daemon crash,
  watchdog reset, malformed control messages, and USB/Ethernet disconnects.
- Confirm serial recovery and restoration of the preserved factory media.
- Validate DDR geometry, GEM/PHY timing, USB peripheral operation, AD9361 probe,
  PLL lock, channel order, sample counter continuity, DMA coherency, and every
  discontinuity counter.
- Archive routed timing/CDC reports and the exact board-build manifest with the
  hardware test report.
