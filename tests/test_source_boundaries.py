"""Repository split, build-stage, and no-flash source assertions."""

import json
from pathlib import Path
import unittest

from neptunesdr_firmwave.board_build import FORBIDDEN_EXECUTABLES


ROOT = Path(__file__).resolve().parents[1]


class SourceBoundaryTests(unittest.TestCase):
    def test_no_twin_python_namespace_reference_remains(self):
        forbidden = "neptunesdr" + "_twin"
        suffixes = {".py", ".sh", ".c", ".md", ".json", ".toml", ".yml", ".yaml"}
        offenders = []
        for path in ROOT.rglob("*"):
            if not path.is_file() or ".git" in path.relative_to(ROOT).parts or path.suffix not in suffixes:
                continue
            if forbidden in path.read_text(encoding="utf-8", errors="replace"):
                offenders.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(offenders, [])

    def test_bundle_runs_build_fetch_validate_xsa_then_prepare(self):
        source = (ROOT / "scripts" / "build_bundle.sh").read_text(encoding="utf-8")
        stages = (
            "build_guest_fft.sh",
            "fetch_firmware.py",
            "test_firmware.py",
            "validate-xsa",
            "prepare_runtime.py",
        )
        positions = [source.index(stage) for stage in stages]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("--fft-streamer", source)

    def test_executable_tooling_has_no_flash_write_path(self):
        # The board orchestrator must name dangerous writers in its fail-closed
        # deny-list.  Exclude that defensive vocabulary from the text scan so a
        # guard cannot be mistaken for an invocation path.
        board_guard = ROOT / "src" / "neptunesdr_firmwave" / "board_build.py"
        payload = "\n".join(
            path.read_text(encoding="utf-8", errors="replace")
            for directory in (ROOT / "src", ROOT / "scripts")
            for path in directory.rglob("*")
            if path.is_file()
            and path.suffix in {".py", ".sh"}
            and path != board_guard
        ).lower()
        for forbidden in ("dfu-util", "flashcp", "mtd_debug write", "dd of=/dev/"):
            self.assertNotIn(forbidden, payload)
        self.assertTrue({"dd", "dfu-util", "flashcp", "mtd_debug"} <= FORBIDDEN_EXECUTABLES)

    def test_interface_and_runtime_locks_are_valid_json_objects(self):
        paths = (
            ROOT / "specs" / "p210-firmware-interface-v1.json",
            ROOT / "src" / "neptunesdr_firmwave" / "data" / "firmware-lock.json",
            ROOT / "src" / "neptunesdr_firmwave" / "data" / "runtime-lock.json",
        )
        for path in paths:
            self.assertIsInstance(json.loads(path.read_text(encoding="utf-8")), dict, path)


if __name__ == "__main__":
    unittest.main()
