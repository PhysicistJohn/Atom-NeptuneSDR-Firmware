"""Command-line entry point for Firmwave artifact and interface tooling."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import List, Optional

from .boot_harness import fetch_locked_to_cache, locked_artifact_path, verify_locked_artifact
from .errors import FirmwaveError
from .interface import interface_path, interface_sha256, load_interface
from .locks import lock_summary, validate_firmware_lock
from .provenance import source_identity
from .version import __version__
from .xsa import validate_xsa


def _json(value: object) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


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
    except (FirmwaveError, OSError, ValueError, KeyError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    raise AssertionError("unhandled command")


__all__ = ["build_parser", "main"]
