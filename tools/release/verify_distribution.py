#!/usr/bin/env python3
"""Verify that built wheel and sdist contain their promised artifact classes."""

from __future__ import annotations

import argparse
from pathlib import Path
import tarfile
import zipfile


# Keep the declared-profile lists aligned with ``[tool.setuptools.data-files]``
# in pyproject.toml.  tests/test_distribution.py enforces that relationship so
# adding an artifact to the wheel declaration cannot silently weaken this gate.
PACKAGE_WHEEL_SUFFIXES = (
    "neptunesdr_firmwave/__init__.py",
    "neptunesdr_firmwave/board_build.py",
    "neptunesdr_firmwave/calibration.py",
    "neptunesdr_firmwave/hardware_manifest.py",
    "neptunesdr_firmwave/provenance.py",
    "neptunesdr_firmwave/data/firmware-lock.json",
    "neptunesdr_firmwave/data/p210-hardware-candidate-v1.json",
    "neptunesdr_firmwave/data/runtime-lock.json",
)

DECLARED_PROFILE_WHEEL_SUFFIXES = (
    "share/neptunesdr-firmwave/specs/neptune-calibration-bundle-v1.json",
    "share/neptunesdr-firmwave/specs/neptune-edge-control-v1.json",
    "share/neptunesdr-firmwave/specs/neptune-edge-data-v1.json",
    "share/neptunesdr-firmwave/specs/neptune-pl-registers-v1.json",
    "share/neptunesdr-firmwave/specs/neptune-update-manifest-v1.json",
    "share/neptunesdr-firmwave/specs/p210-firmware-interface-v1.json",
    "share/neptunesdr-firmwave/specs/p210-hardware-manifest-v1.schema.json",
    "share/neptunesdr-firmwave/docs/ACCEPTANCE.md",
    "share/neptunesdr-firmwave/docs/BOARD_BUILD.md",
    "share/neptunesdr-firmwave/docs/CALIBRATION.md",
    "share/neptunesdr-firmwave/docs/HARDWARE_EVIDENCE.md",
    "share/neptunesdr-firmwave/docs/NEPTUNE_EDGE_ARCHITECTURE.md",
    "share/neptunesdr-firmwave/docs/P210_FFT_ABI.md",
    "share/neptunesdr-firmwave/docs/PROVENANCE.md",
    "share/neptunesdr-firmwave/docs/RECOVERY.md",
    "share/neptunesdr-firmwave/board/p210/README.md",
    "share/neptunesdr-firmwave/board/p210/build-plan.json",
    "share/neptunesdr-firmwave/board/p210/profile.json",
    "share/neptunesdr-firmwave/board/p210/source-lock.json",
    "share/neptunesdr-firmwave/board/p210/toolchain-lock.json",
    "share/neptunesdr-firmwave/board/p210/config/calibration-defaults.json",
    "share/neptunesdr-firmwave/board/p210/config/device-policy.json",
    "share/neptunesdr-firmwave/board/p210/config/kernel-required.json",
    "share/neptunesdr-firmwave/board/p210/config/rootfs-policy.json",
    "share/neptunesdr-firmwave/board/p210/config/u-boot.env",
    "share/neptunesdr-firmwave/protocol/README.md",
    "share/neptunesdr-firmwave/protocol/generated/neptune_edge_profile_v1.json",
    "share/neptunesdr-firmwave/protocol/generated/neptune_edge_v1.h",
    "share/neptunesdr-firmwave/protocol/generated/neptune_edge_v1.py",
    "share/neptunesdr-firmwave/protocol/generated/neptune_pl_registers_v1.h",
    "share/neptunesdr-firmwave/ps/include/neptune_pl_registers_v1.h",
    "share/neptunesdr-firmwave/protocol/golden/neptune_edge_v1_vectors.json",
    "share/neptunesdr-firmwave/ps/include/neptune_stream_uapi.h",
    "share/neptunesdr-firmwave/hdl/rtl/generated/neptune_pl_registers_v1_pkg.sv",
)

DECLARED_PROFILE_SOURCE_SUFFIXES = (
    "specs/neptune-calibration-bundle-v1.json",
    "specs/neptune-edge-control-v1.json",
    "specs/neptune-edge-data-v1.json",
    "specs/neptune-pl-registers-v1.json",
    "specs/neptune-update-manifest-v1.json",
    "specs/p210-firmware-interface-v1.json",
    "specs/p210-hardware-manifest-v1.schema.json",
    "docs/ACCEPTANCE.md",
    "docs/BOARD_BUILD.md",
    "docs/CALIBRATION.md",
    "docs/HARDWARE_EVIDENCE.md",
    "docs/NEPTUNE_EDGE_ARCHITECTURE.md",
    "docs/P210_FFT_ABI.md",
    "docs/PROVENANCE.md",
    "docs/RECOVERY.md",
    "board/p210/README.md",
    "board/p210/build-plan.json",
    "board/p210/profile.json",
    "board/p210/source-lock.json",
    "board/p210/toolchain-lock.json",
    "board/p210/config/calibration-defaults.json",
    "board/p210/config/device-policy.json",
    "board/p210/config/kernel-required.json",
    "board/p210/config/rootfs-policy.json",
    "board/p210/config/u-boot.env",
    "protocol/README.md",
    "protocol/generated/neptune_edge_profile_v1.json",
    "protocol/generated/neptune_edge_v1.h",
    "protocol/generated/neptune_edge_v1.py",
    "protocol/generated/neptune_pl_registers_v1.h",
    "ps/include/neptune_pl_registers_v1.h",
    "protocol/golden/neptune_edge_v1_vectors.json",
    "ps/include/neptune_stream_uapi.h",
    "hdl/rtl/generated/neptune_pl_registers_v1_pkg.sv",
)

WHEEL_SUFFIXES = PACKAGE_WHEEL_SUFFIXES + DECLARED_PROFILE_WHEEL_SUFFIXES

SDIST_SUFFIXES = PACKAGE_WHEEL_SUFFIXES + DECLARED_PROFILE_SOURCE_SUFFIXES + (
    "LICENSE",
    "NOTICE.md",
    "README.md",
    "MANIFEST.in",
    "pyproject.toml",
    "hdl/rtl/core/neptune_rx_pipeline.sv",
    "hdl/tb/tb_neptune_s8bf_revision_boundary.sv",
    "ps/control-daemon/src/necp.c",
    "ps/data-service/src/service.c",
    "ps/kernel/neptune_stream/neptune_stream.c",
    "tools/host/Makefile",
    "tools/release/verify_distribution.py",
    "tools/vivado/vm.sh",
    "scripts/check.sh",
    "LICENSES/BSD-2-Clause.txt",
    "LICENSES/GPL-2.0-or-later.txt",
)


def _require_suffixes(names, suffixes, label):
    missing = [suffix for suffix in suffixes if not any(name.endswith(suffix) for name in names)]
    if missing:
        raise ValueError("%s is missing: %s" % (label, ", ".join(missing)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wheel", type=Path)
    parser.add_argument("sdist", type=Path)
    args = parser.parse_args()
    with zipfile.ZipFile(args.wheel) as archive:
        wheel_names = tuple(archive.namelist())
        _require_suffixes(wheel_names, WHEEL_SUFFIXES, "wheel")
        if any(name.endswith((".o", ".ko", ".vvp")) for name in wheel_names):
            raise ValueError("wheel contains a local compiled artifact")
    with tarfile.open(args.sdist, mode="r:gz") as archive:
        sdist_names = tuple(member.name for member in archive.getmembers())
        _require_suffixes(sdist_names, SDIST_SUFFIXES, "sdist")
        if any(name.endswith((".o", ".ko", ".vvp")) for name in sdist_names):
            raise ValueError("sdist contains a local compiled artifact")
    print("NEPTUNE_DISTRIBUTION PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
