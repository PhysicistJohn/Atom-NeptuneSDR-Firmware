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

from neptunesdr_firmwave.cli import main
from neptunesdr_firmwave.provenance import _source_state, source_identity


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
    submodules = run(("git", "submodule", "status", "--recursive"))
    material = {
        "commit": commit,
        "branch": branch,
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "tracked_diff_bytes": len(diff),
        "untracked": untracked,
        "submodule_status_sha256": hashlib.sha256(submodules).hexdigest(),
        "submodule_status": submodules.decode("utf-8", "replace").splitlines(),
    }
    encoded = json.dumps(material, sort_keys=True, separators=(",", ":")).encode()
    material["state_sha256"] = hashlib.sha256(encoded).hexdigest()
    material["clean"] = not diff and not untracked and not submodules.strip()
    return material


def _repository(root: Path) -> None:
    subprocess.run(("git", "init", "-b", "main"), cwd=root, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(("git", "config", "user.name", "Firmwave Test"), cwd=root, check=True)
    subprocess.run(("git", "config", "user.email", "firmwave@example.invalid"), cwd=root, check=True)
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
        self.assertEqual(identity["repository"], "Atom-NeptuneSDR_Firmwave")
        self.assertEqual(identity["tree"], tree)
        self.assertTrue(identity["clean"])
        self.assertRegex(identity["state_sha256"], r"^[0-9a-f]{64}$")


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


if __name__ == "__main__":
    unittest.main()
