"""Source identity parity and public command-line surface."""

from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from neptunesdr_firmware.cli import main
from neptunesdr_firmware.provenance import _source_state, source_identity


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _twin_acceptance_source_state(root: Path):
    """Independent copy of the consuming acceptance algorithm."""

    run = lambda arguments: subprocess.check_output(arguments, cwd=str(root), stderr=subprocess.STDOUT)
    commit = run(("git", "rev-parse", "HEAD")).decode().strip()
    branch = run(("git", "rev-parse", "--abbrev-ref", "HEAD")).decode().strip()
    diff = run(("git", "diff", "--binary", "--no-ext-diff", "HEAD", "--"))
    untracked_raw = run(("git", "ls-files", "--others", "--exclude-standard", "-z"))
    index_tags_raw = run(("git", "ls-files", "-v", "-z"))
    untracked = []
    for encoded in untracked_raw.split(b"\0"):
        if not encoded:
            continue
        relative = os.fsdecode(encoded)
        candidate = root / relative
        if candidate.is_file():
            untracked.append(
                {"path": relative, "bytes": candidate.stat().st_size, "sha256": _sha256_file(candidate)}
            )
        else:
            untracked.append({"path": relative, "type": "non-regular"})
    hidden_index_flags = []
    for encoded in index_tags_raw.split(b"\0"):
        if not encoded:
            continue
        tag = chr(encoded[0])
        if tag == "S" or tag.islower():
            hidden_index_flags.append({"path": os.fsdecode(encoded[2:]), "tag": tag})
    submodules = run(("git", "submodule", "status", "--recursive"))
    material = {
        "commit": commit,
        "branch": branch,
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "tracked_diff_bytes": len(diff),
        "hidden_index_flags": hidden_index_flags,
        "untracked": untracked,
        "submodule_status_sha256": hashlib.sha256(submodules).hexdigest(),
        "submodule_status": submodules.decode("utf-8", "replace").splitlines(),
    }
    encoded = json.dumps(material, sort_keys=True, separators=(",", ":")).encode()
    material["state_sha256"] = hashlib.sha256(encoded).hexdigest()
    material["clean"] = (
        not diff
        and not hidden_index_flags
        and not untracked
        and not submodules.strip()
    )
    return material


def _repository(root: Path) -> None:
    subprocess.run(("git", "init", "-b", "main"), cwd=root, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(("git", "config", "user.name", "Firmware Test"), cwd=root, check=True)
    subprocess.run(("git", "config", "user.email", "firmware@example.invalid"), cwd=root, check=True)
    (root / "tracked.bin").write_bytes(b"tracked\0bytes")
    subprocess.run(("git", "add", "tracked.bin"), cwd=root, check=True)
    subprocess.run(("git", "commit", "-m", "fixture"), cwd=root, check=True, stdout=subprocess.DEVNULL)


class ProvenanceTests(unittest.TestCase):
    def test_state_sha_exactly_matches_twin_acceptance_material_clean_and_dirty(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _repository(root)
            self.assertEqual(_source_state(root), _twin_acceptance_source_state(root))
            (root / "tracked.bin").write_bytes(b"changed\0bytes")
            (root / "untracked.bin").write_bytes(b"new\0bytes")
            self.assertEqual(_source_state(root), _twin_acceptance_source_state(root))

    def test_source_identity_adds_exact_repository_and_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _repository(root)
            identity = source_identity(root)
            tree = subprocess.check_output(("git", "rev-parse", "HEAD^{tree}"), cwd=root, text=True).strip()
        self.assertEqual(identity["repository"], "Atom-NeptuneSDR-Firmware")
        self.assertEqual(identity["tree"], tree)
        self.assertTrue(identity["clean"])
        self.assertRegex(identity["state_sha256"], r"^[0-9a-f]{64}$")

    def test_hidden_index_flags_cannot_mask_worktree_changes(self):
        cases = (
            ("--skip-worktree", "S"),
            ("--assume-unchanged", "h"),
        )
        for flag, expected_tag in cases:
            with self.subTest(flag=flag), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                _repository(root)
                clean_state = _source_state(root)
                subprocess.run(
                    ("git", "update-index", flag, "tracked.bin"),
                    cwd=root,
                    check=True,
                )
                (root / "tracked.bin").write_bytes(b"hidden change")
                hidden_state = _source_state(root)
                self.assertFalse(hidden_state["clean"])
                self.assertEqual(
                    hidden_state["hidden_index_flags"],
                    [{"path": "tracked.bin", "tag": expected_tag}],
                )
                self.assertNotEqual(
                    hidden_state["state_sha256"], clean_state["state_sha256"]
                )


class CLITests(unittest.TestCase):
    def _run(self, arguments):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = main(arguments)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_interface_json_is_machine_readable_and_non_flashable(self):
        code, output, error = self._run(["interface", "--json"])
        self.assertEqual((code, error), (0, ""))
        value = json.loads(output)
        self.assertEqual(value["profile"], "qemu-development")
        self.assertIs(value["flashable"], False)

    def test_validate_locks_succeeds_without_network(self):
        code, output, error = self._run(["validate-locks", "--json"])
        self.assertEqual((code, error), (0, ""))
        self.assertTrue(json.loads(output)["valid"])

    def test_fetch_unknown_artifact_fails_before_network(self):
        code, output, error = self._run(["fetch", "unknown"])
        self.assertEqual(code, 2)
        self.assertEqual(output, "")
        self.assertIn("unknown locked artifact", error)

    def test_source_identity_outside_git_fails_without_a_traceback(self):
        with tempfile.TemporaryDirectory() as directory:
            code, output, error = self._run(
                ["source-identity", "--root", directory, "--json"]
            )
        self.assertEqual(code, 2)
        self.assertEqual(output, "")
        self.assertIn("source identity requires a Git checkout", error)
        self.assertNotIn("Traceback", error)


if __name__ == "__main__":
    unittest.main()
