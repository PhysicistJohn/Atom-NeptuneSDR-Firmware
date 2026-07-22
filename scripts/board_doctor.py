#!/usr/bin/env python3
"""Validate the physical P210 build definition and its local prerequisites."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import List, Optional


REPOSITORY = Path(__file__).resolve().parents[1]
SOURCE_TREE = REPOSITORY / "src"
if str(SOURCE_TREE) not in sys.path:
    sys.path.insert(0, str(SOURCE_TREE))

from neptunesdr_firmwave.board_build import doctor_board  # noqa: E402
from neptunesdr_firmwave.errors import BoardBuildError  # noqa: E402


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Validate the fail-closed P210 physical-board build definition. "
            "This command never contacts or writes a board."
        )
    )
    parser.add_argument(
        "--board-dir",
        type=Path,
        default=REPOSITORY / "board" / "p210",
        help="board definition containing source/toolchain/profile/build-plan JSON",
    )
    parser.add_argument(
        "--repository",
        type=Path,
        default=REPOSITORY,
        help="Firmwave source checkout used to resolve locked source paths",
    )
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="validate structure and report unresolved selections without probing installed tools",
    )
    parser.add_argument("--json", action="store_true", help="emit a machine-readable report")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = _parser().parse_args(argv)
    try:
        report = doctor_board(
            args.board_dir,
            args.repository,
            check_environment=not args.validate_only,
        )
    except (BoardBuildError, OSError, ValueError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print("profile=%s" % report["profile"])
        print("platform=%s" % report["platform_id"])
        print("definition=valid")
        print("build_ready=%s" % ("yes" if report["ready"] else "no"))
        print("qspi_write_allowed=no")
        print("tx_enabled_at_boot=no")
        for blocker in report["blockers"]:
            print("BLOCKED: %s" % blocker)
    if args.validate_only:
        return 0
    return 0 if report["ready"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
