"""Physical-board scaffold validation, safety, and manifest coverage."""

import copy
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile
import unittest

from neptunesdr_firmwave.board_build import (
    BOARD_MANIFEST_SCHEMA,
    REQUIRED_OUTPUT_IDS,
    REQUIRED_SOURCE_IDS,
    build_board,
    doctor_board,
    load_board_definition,
    validate_build_plan,
    validate_source_lock,
)
from neptunesdr_firmwave.errors import BoardBuildError


ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "board" / "p210"


def _write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def _locked_fixture(destination):
    repository = destination / "repository"
    repository.mkdir()
    subprocess.run(("git", "init", "-b", "main"), cwd=repository, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(("git", "config", "user.name", "Board Build Test"), cwd=repository, check=True)
    subprocess.run(
        ("git", "config", "user.email", "board-build@example.invalid"),
        cwd=repository,
        check=True,
    )
    (repository / "source.txt").write_text("locked source\n", encoding="utf-8")
    (repository / "build_stage.py").write_text(
        "import json,os,pathlib,sys\n"
        "root=pathlib.Path(os.environ['NEPTUNE_BOARD_OUTPUT'])\n"
        "for value in json.loads(sys.argv[1]):\n"
        " path=root/value; path.parent.mkdir(parents=True,exist_ok=True); "
        "path.write_bytes(value.encode('utf-8'))\n",
        encoding="utf-8",
    )
    subprocess.run(("git", "add", "source.txt", "build_stage.py"), cwd=repository, check=True)
    subprocess.run(
        ("git", "commit", "-m", "fixture"),
        cwd=repository,
        check=True,
        stdout=subprocess.DEVNULL,
    )
    board = destination / "board"
    board.mkdir()
    profile = json.loads((BOARD / "profile.json").read_text(encoding="utf-8"))
    base_plan = json.loads((BOARD / "build-plan.json").read_text(encoding="utf-8"))
    commit = subprocess.check_output(("git", "rev-parse", "HEAD"), cwd=repository).decode().strip()
    sources = {}
    for name in REQUIRED_SOURCE_IDS:
        sources[name] = {
            "status": "locked",
            "platform_id": profile["platform_id"],
            "kind": "git",
            "url": "https://example.invalid/%s.git" % name,
            "commit": commit,
            "path": ".",
        }
    source_lock = {
        "schema": "neptunesdr.board-source-lock/v1",
        "profile": profile["profile"],
        "platform_id": profile["platform_id"],
        "sources": sources,
    }
    toolchain = {
        "schema": "neptunesdr.board-toolchain-lock/v1",
        "platform_id": profile["platform_id"],
        "tools": {
            "python": {
                "status": "locked",
                "command": sys.executable,
                "version_args": ["--version"],
                "version_regex": r"Python %s" % re.escape(platform.python_version()),
            }
        },
    }
    plan = copy.deepcopy(base_plan)
    for stage in plan["stages"]:
        paths = [
            next(item["path"] for item in profile["outputs"] if item["id"] == output)
            for output in stage["outputs"]
        ]
        stage["status"] = "locked"
        stage.pop("reason", None)
        stage["command"] = [
            sys.executable,
            str(repository / "build_stage.py"),
            json.dumps(paths),
        ]
    for name, value in (
        ("source-lock.json", source_lock),
        ("toolchain-lock.json", toolchain),
        ("profile.json", profile),
        ("build-plan.json", plan),
    ):
        _write_json(board / name, value)
    return board, repository


class CheckedInBoardDefinitionTests(unittest.TestCase):
    def test_definition_is_complete_safe_and_deliberately_blocked(self):
        definition = load_board_definition(BOARD)
        profile = definition["profile"]
        self.assertEqual(profile["profile"], "p210-sd-development-v0")
        self.assertIs(profile["flashable"], False)
        self.assertEqual(profile["installation"]["medium"], "removable-sd")
        self.assertIs(profile["installation"]["host_writer_included"], False)
        self.assertIs(profile["installation"]["qspi_write_allowed"], False)
        self.assertIs(profile["safety"]["tx_enabled_at_boot"], False)
        self.assertEqual(
            tuple(item["id"] for item in profile["outputs"]),
            REQUIRED_OUTPUT_IDS,
        )

        report = doctor_board(BOARD, ROOT, check_environment=False)
        self.assertTrue(report["valid"])
        self.assertFalse(report["ready"])
        self.assertGreaterEqual(len(report["blockers"]), len(REQUIRED_SOURCE_IDS))
        self.assertIn("sd-image", report["stage_order"])

    def test_validate_only_cli_succeeds_without_proprietary_tools(self):
        result = subprocess.run(
            (
                sys.executable,
                str(ROOT / "scripts" / "board_doctor.py"),
                "--validate-only",
                "--json",
            ),
            cwd=ROOT,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report["valid"])
        self.assertFalse(report["ready"])

    def test_checked_in_builder_refuses_unresolved_inputs_before_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "must-not-exist"
            with self.assertRaisesRegex(BoardBuildError, "board build is blocked"):
                build_board(BOARD, ROOT, output)
            self.assertFalse(output.exists())


class BoardDefinitionSafetyTests(unittest.TestCase):
    def test_source_platform_mismatch_is_rejected(self):
        lock = json.loads((BOARD / "source-lock.json").read_text(encoding="utf-8"))
        lock["sources"]["linux"]["platform_id"] = "another-platform"
        with self.assertRaisesRegex(BoardBuildError, "not matched"):
            validate_source_lock(lock)

    def test_build_plan_rejects_device_writers(self):
        profile = json.loads((BOARD / "profile.json").read_text(encoding="utf-8"))
        plan = json.loads((BOARD / "build-plan.json").read_text(encoding="utf-8"))
        stage = plan["stages"][0]
        stage["status"] = "locked"
        stage.pop("reason")
        stage["command"] = ["dd", "if=BOOT.BIN", "/dev/sda"]
        with self.assertRaisesRegex(BoardBuildError, "forbidden writer"):
            validate_build_plan(plan, profile["platform_id"], profile["profile"])

    def test_build_plan_rejects_shell_and_inline_interpreter_escape_hatches(self):
        profile = json.loads((BOARD / "profile.json").read_text(encoding="utf-8"))
        for command, message in (
            (["sh", "-c", "touch output"], "launcher"),
            ([sys.executable, "-c", "print('unreviewed')"], "inline"),
            ([sys.executable, "stage.py", "open('/dev/sda','wb')"], "device node"),
        ):
            with self.subTest(command=command):
                plan = json.loads((BOARD / "build-plan.json").read_text(encoding="utf-8"))
                stage = plan["stages"][0]
                stage["status"] = "locked"
                stage.pop("reason")
                stage["command"] = command
                with self.assertRaisesRegex(BoardBuildError, message):
                    validate_build_plan(plan, profile["platform_id"], profile["profile"])


class BoardBuildManifestTests(unittest.TestCase):
    def test_fully_locked_fixture_build_is_deterministic_and_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            board, repository = _locked_fixture(root)
            report = doctor_board(board, repository, check_environment=True)
            self.assertTrue(report["ready"], report["blockers"])

            first, first_path = build_board(board, repository, root / "output-a")
            second, second_path = build_board(board, repository, root / "output-b")
            self.assertEqual(first, second)
            self.assertEqual(first_path.read_bytes(), second_path.read_bytes())
            self.assertEqual(first["schema"], BOARD_MANIFEST_SCHEMA)
            self.assertFalse(first["flashable"])
            self.assertFalse(first["physical_validation"])
            self.assertFalse(first["safety"]["tx_enabled_at_boot"])
            self.assertFalse(first["installation"]["qspi_write_allowed"])
            self.assertTrue(first["artifact_hashes_complete"])
            self.assertEqual(tuple(first["generated_artifacts"]), REQUIRED_OUTPUT_IDS)
            for name, record in first["generated_artifacts"].items():
                with self.subTest(artifact=name):
                    self.assertRegex(record["sha256"], r"^[0-9a-f]{64}$")
                    self.assertGreater(record["bytes"], 0)
                    self.assertFalse(Path(record["path"]).is_absolute())

    def test_fully_locked_fixture_refuses_device_node_output(self):
        if not Path("/dev/null").exists():
            self.skipTest("platform has no /dev/null")
        with tempfile.TemporaryDirectory() as directory:
            board, repository = _locked_fixture(Path(directory))
            with self.assertRaisesRegex(BoardBuildError, "refusing a broad"):
                build_board(board, repository, Path("/dev/null"))

    def test_fully_locked_fixture_rejects_dirty_source_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            board, repository = _locked_fixture(Path(directory))
            (repository / "untracked.txt").write_text("dirty\n", encoding="utf-8")
            report = doctor_board(board, repository, check_environment=True)
            self.assertFalse(report["ready"])
            self.assertTrue(
                any("not clean at its locked commit" in item for item in report["blockers"])
            )

    def test_fully_locked_fixture_rejects_wrong_tool_version(self):
        with tempfile.TemporaryDirectory() as directory:
            board, repository = _locked_fixture(Path(directory))
            toolchain_path = board / "toolchain-lock.json"
            toolchain = json.loads(toolchain_path.read_text(encoding="utf-8"))
            toolchain["tools"]["python"]["version_regex"] = "not-a-real-python-version"
            _write_json(toolchain_path, toolchain)
            report = doctor_board(board, repository, check_environment=True)
            self.assertFalse(report["ready"])
            self.assertIn("tool python does not match its locked version", report["blockers"])


if __name__ == "__main__":
    unittest.main()
