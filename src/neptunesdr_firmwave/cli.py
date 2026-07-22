"""Command-line entry point for Firmwave artifact and interface tooling."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import List, Optional

from .boot_harness import fetch_locked_to_cache, locked_artifact_path, verify_locked_artifact
from .calibration import validate_bundle
from .errors import FirmwaveError
from .hardware_manifest import validate_hardware_manifest
from .interface import interface_path, interface_sha256, load_interface
from .locks import lock_summary, validate_firmware_lock
from .provenance import source_identity
from .update_manifest import validate_update_manifest
from .version import __version__
from .xsa import validate_xsa


def _json(value: object) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


def _load_json_object(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid JSON in %s: %s" % (path, exc)) from exc


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="neptune-firmwave",
        description="Audited P210 firmware inputs and a non-flashable QEMU development runtime",
    )
    parser.add_argument("--version", action="version", version="%(prog)s " + __version__)
    commands = parser.add_subparsers(dest="command", required=True)

    interface = commands.add_parser("interface", help="show the canonical Twin/Firmwave interface")
    interface.add_argument("--path", type=Path)
    interface.add_argument("--json", action="store_true")

    source = commands.add_parser("source-identity", help="identify the exact Firmwave source state")
    source.add_argument("--root", type=Path)
    source.add_argument("--json", action="store_true")

    locks = commands.add_parser("validate-locks", help="validate firmware and runtime locks")
    locks.add_argument("--firmware-lock", type=Path)
    locks.add_argument("--runtime-lock", type=Path)
    locks.add_argument("--json", action="store_true")

    fetch = commands.add_parser("fetch", help="fetch content-addressed inputs; never flash")
    fetch.add_argument("artifacts", nargs="*")
    fetch.add_argument("--cache-dir", type=Path, default=Path(".cache/firmware"))
    fetch.add_argument("--lock", type=Path)
    fetch.add_argument("--force", action="store_true")
    fetch.add_argument("--json", action="store_true")

    xsa = commands.add_parser("validate-xsa", help="validate a P210 Xilinx hardware handoff")
    selection = xsa.add_mutually_exclusive_group(required=True)
    selection.add_argument("source", nargs="?", type=Path)
    selection.add_argument("--artifact", metavar="NAME")
    xsa.add_argument("--cache-dir", type=Path, default=Path(".cache/firmware"))
    xsa.add_argument("--lock", type=Path)
    xsa.add_argument("--json", action="store_true")

    hardware = commands.add_parser(
        "validate-hardware-manifest",
        help="cross-check a non-flashable P210 hardware-evidence candidate",
    )
    hardware.add_argument("--manifest", type=Path)
    hardware.add_argument("--firmware-lock", type=Path)
    hardware.add_argument("--json", action="store_true")

    calibration = commands.add_parser(
        "validate-calibration",
        help="validate an integrity-checked, device-bound calibration bundle",
    )
    calibration.add_argument("bundle", type=Path)
    calibration.add_argument("--device-serial")
    calibration.add_argument("--json", action="store_true")

    update = commands.add_parser(
        "validate-update",
        help="verify a signed, rollback-resistant, SD-only update manifest",
    )
    update.add_argument("manifest", type=Path)
    update.add_argument("--artifact-root", type=Path, required=True)
    update.add_argument("--public-key", type=Path, required=True)
    update.add_argument("--accepted-rollback-index", type=int, required=True)
    update.add_argument("--openssl", default="openssl")
    update.add_argument("--json", action="store_true")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "interface":
            value = dict(load_interface(args.path))
            if args.json:
                _json(value)
            else:
                print("schema=%s" % value["schema"])
                print("profile=%s" % value["profile"])
                print("flashable=no")
                print("sha256=%s" % interface_sha256(args.path))
                print("path=%s" % interface_path(args.path))
            return 0
        if args.command == "source-identity":
            value = source_identity(args.root)
            if args.json:
                _json(value)
            else:
                for key in ("repository", "commit", "tree", "clean", "state_sha256"):
                    print("%s=%s" % (key, value[key]))
            return 0
        if args.command == "validate-locks":
            value = lock_summary(args.firmware_lock, args.runtime_lock)
            if args.json:
                _json(value)
            else:
                print("Firmwave locks: PASS (%d artifacts)" % len(value["artifacts"]))
            return 0
        if args.command == "fetch":
            lock = validate_firmware_lock(args.lock)
            names = args.artifacts or sorted(lock["artifacts"])
            unknown = sorted(set(names) - set(lock["artifacts"]))
            if unknown:
                raise ValueError("unknown locked artifact(s): %s" % ", ".join(unknown))
            results = []
            for name in names:
                path = fetch_locked_to_cache(name, args.cache_dir, args.lock, force=args.force)
                results.append(
                    {
                        "name": name,
                        "path": str(path.resolve()),
                        "sha256": verify_locked_artifact(name, path, args.lock),
                        "bytes": path.stat().st_size,
                    }
                )
            if args.json:
                _json(results)
            else:
                for item in results:
                    print("{name}\t{sha256}\t{bytes}\t{path}".format(**item))
            return 0
        if args.command == "validate-xsa":
            source = args.source
            if args.artifact is not None:
                path = locked_artifact_path(args.artifact, args.cache_dir, args.lock)
                verify_locked_artifact(args.artifact, path, args.lock)
                source = path
            report = validate_xsa(source)
            if args.json:
                _json(report.to_dict())
            else:
                print("P210 XSA: %s" % ("PASS" if report.compatible else "FAIL"))
            return 0 if report.compatible else 1
        if args.command == "validate-hardware-manifest":
            report = validate_hardware_manifest(args.manifest, args.firmware_lock)
            if args.json:
                _json(report.to_dict())
            else:
                print(
                    "P210 hardware evidence: %s (%d issue(s))"
                    % ("PASS" if report.compatible else "FAIL", len(report.issues))
                )
                print("candidate=%s" % report.candidate_id)
                print("source=%s" % report.source)
            return 0 if report.compatible else 1
        if args.command == "validate-calibration":
            value = validate_bundle(
                _load_json_object(args.bundle), expected_serial=args.device_serial
            )
            if args.json:
                _json(value)
            else:
                print("Calibration bundle: PASS")
                print("device_serial=%s" % value["device_serial"])
                print("revision=%d" % value["revision"])
                print("tables=%d" % len(value["tables"]))
            return 0
        if args.command == "validate-update":
            value = validate_update_manifest(
                _load_json_object(args.manifest),
                args.artifact_root,
                public_key=args.public_key,
                accepted_rollback_index=args.accepted_rollback_index,
                openssl=args.openssl,
            )
            if args.json:
                _json(value)
            else:
                print("Signed SD update: PASS")
                print("platform_id=%s" % value["platform_id"])
                print("target_slot=%s" % value["target_slot"])
                print("rollback_index=%d" % value["rollback_index"])
            return 0
    except (FirmwaveError, OSError, ValueError, KeyError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    raise AssertionError("unhandled command")


__all__ = ["build_parser", "main"]
