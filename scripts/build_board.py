#!/usr/bin/env python3
"""Execute a fully locked P210 SD-only build plan; never write block devices."""

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

from neptunesdr_firmwave.board_build import build_board, doctor_board  # noqa: E402
from neptunesdr_firmwave.errors import BoardBuildError  # noqa: E402


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run the deterministic P210 physical-board build plan and create a "
            "regular-file SD image plus a complete manifest. No device writer "
            "or QSPI update path is implemented."
        )
    )
    parser.add_argument(
        "--board-dir",
        type=Path,
        default=REPOSITORY / "board" / "p210",
    )
    parser.add_argument("--repository", type=Path, default=REPOSITORY)
    parser.add_argument(
        "--output",
        type=Path,
        default=REPOSITORY / ".cache" / "p210-board",
        help="regular output directory; never a block device",
    )
    parser.add_argument(
        "--plan",
        action="store_true",
        help="show the structurally validated stage order and blockers without writing files",
    )
    parser.add_argument("--json", action="store_true", help="emit the plan or final manifest as JSON")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.plan:
            report = doctor_board(args.board_dir, args.repository, check_environment=False)
            if args.json:
                print(json.dumps(report, indent=2, sort_keys=True))
            else:
                print("profile=%s" % report["profile"])
                print("stages=%s" % ",".join(report["stage_order"]))
                print("build_ready=%s" % ("yes" if report["ready"] else "no"))
                for blocker in report["blockers"]:
                    print("BLOCKED: %s" % blocker)
            return 0
        manifest, manifest_path = build_board(args.board_dir, args.repository, args.output)
    except (BoardBuildError, OSError, ValueError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(manifest, indent=2, sort_keys=True))
    else:
        print("profile=%s" % manifest["profile"])
        print("physical_validation=no")
        print("flashable=no")
        print("qspi_write_allowed=no")
        print("manifest=%s" % manifest_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
