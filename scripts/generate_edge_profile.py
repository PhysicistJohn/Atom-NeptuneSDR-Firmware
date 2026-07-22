#!/usr/bin/env python3
"""Generate the single hashed Neptune Edge custom-firmware interface profile."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "protocol" / "generated" / "neptune_edge_profile_v1.json"

ARTIFACTS = {
    "data_schema": "specs/neptune-edge-data-v1.json",
    "control_schema": "specs/neptune-edge-control-v1.json",
    "pl_register_schema": "specs/neptune-pl-registers-v1.json",
    "calibration_schema": "specs/neptune-calibration-bundle-v1.json",
    "update_schema": "specs/neptune-update-manifest-v1.json",
    "hardware_manifest_schema": "specs/p210-hardware-manifest-v1.schema.json",
    "python_binding": "protocol/generated/neptune_edge_v1.py",
    "c_binding": "protocol/generated/neptune_edge_v1.h",
    "pl_userspace_header": "protocol/generated/neptune_pl_registers_v1.h",
    "pl_kernel_header": "ps/include/neptune_pl_registers_v1.h",
    "stream_uapi": "ps/include/neptune_stream_uapi.h",
    "pl_systemverilog_package": "hdl/rtl/generated/neptune_pl_registers_v1_pkg.sv",
    "golden_vectors": "protocol/golden/neptune_edge_v1_vectors.json",
    "board_profile": "board/p210/profile.json",
}


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def render() -> bytes:
    artifacts = {}
    for name, relative in ARTIFACTS.items():
        path = ROOT / relative
        if not path.is_file():
            raise ValueError("profile artifact is absent: %s" % relative)
        artifacts[name] = {
            "path": relative,
            "sha256": _sha256(path),
            "bytes": path.stat().st_size,
        }
    document = {
        "schema": "neptunesdr.edge.firmware-profile/v1",
        "profile": "p210-edge-custom-v0",
        "protocol_version": 1,
        "release_ready": False,
        "physical_validation_required": True,
        "execution_target": "HamGeek Neptune/P210 with XC7Z020 and AD9361",
        "clock_contract": {
            "ingress_sample_rate_hz": 61_440_000,
            "continuous_ethernet_sample_rate_hz": 55_000_000,
            "timestamp_timebase_hz": 61_440_000,
            "resampler_interpolation": 1375,
            "resampler_decimation": 1536,
        },
        "transport_contract": {
            "control": "USB NECP-v1",
            "data": "Gigabit Ethernet UDP NEDP-v1",
            "continuous_baseline": "one RX channel S8/S8BF at 55 MSPS with MTU 9000",
            "onboard_ingress": "full 61.44 MSPS per enabled AD9361 RX channel",
        },
        "safety_contract": {
            "raw_bypass_required": True,
            "tx_enabled_at_boot": False,
            "persistent_tx_inhibit": True,
            "qspi_write_allowed": False,
            "installation_medium": "removable-sd",
        },
        "legacy_compatibility": {
            "profile": "qemu-development",
            "interface": "specs/p210-firmware-interface-v1.json",
            "relationship": "additive maintenance interface; not the Edge high-rate architecture",
        },
        "artifacts": artifacts,
    }
    return (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the checked-in profile is stale")
    args = parser.parse_args()
    try:
        expected = render()
    except (OSError, ValueError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    if args.check:
        try:
            observed = OUTPUT.read_bytes()
        except OSError as exc:
            print("error: cannot read %s: %s" % (OUTPUT.relative_to(ROOT), exc), file=sys.stderr)
            return 1
        if observed != expected:
            print("error: %s is stale; run scripts/generate_edge_profile.py" % OUTPUT.relative_to(ROOT), file=sys.stderr)
            return 1
        print("NEPTUNE_EDGE_PROFILE_V1_GENERATED PASS")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(expected)
    print("wrote %s" % OUTPUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
