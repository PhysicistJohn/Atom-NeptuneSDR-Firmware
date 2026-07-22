"""Reproducible identity for the exact Firmwave source state."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import hashlib
from typing import Any, Dict, List, Optional, Sequence

from .interface import repository_root


REPOSITORY_NAME = "Atom-NeptuneSDR_Firmwave"


def _sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _run_bytes(arguments: Sequence[str], root: Path) -> bytes:
    return subprocess.check_output(arguments, cwd=str(root), stderr=subprocess.STDOUT)


def _source_state(root: Path) -> Dict[str, Any]:
    """Match the consuming Twin's acceptance source-state algorithm exactly."""

    root = root.resolve()
    try:
        commit = _run_bytes(("git", "rev-parse", "HEAD"), root).decode().strip()
        branch = _run_bytes(
            ("git", "rev-parse", "--abbrev-ref", "HEAD"), root
        ).decode().strip()
        diff = _run_bytes(
            ("git", "diff", "--binary", "--no-ext-diff", "HEAD", "--"), root
        )
    except subprocess.CalledProcessError:
        # This exists only so the source gate can run before the repository's
        # initial commit. Once committed, the branch above is byte-for-byte the
        # acceptance algorithm and is covered by a cross-implementation test.
        commit = "unborn"
        branch = _run_bytes(("git", "symbolic-ref", "--short", "HEAD"), root).decode().strip()
        diff = b""
    untracked_raw = _run_bytes(
        ("git", "ls-files", "--others", "--exclude-standard", "-z"), root
    )
    untracked: List[Dict[str, Any]] = []
    for encoded in untracked_raw.split(b"\0"):
        if not encoded:
            continue
        relative = os.fsdecode(encoded)
        candidate = root / relative
        if candidate.is_file():
            untracked.append(
                {
                    "path": relative,
                    "bytes": candidate.stat().st_size,
                    "sha256": _sha256_file(candidate),
                }
            )
        else:
            untracked.append({"path": relative, "type": "non-regular"})
    submodules = _run_bytes(("git", "submodule", "status", "--recursive"), root)
    material: Dict[str, Any] = {
        "commit": commit,
        "branch": branch,
        "tracked_diff_sha256": _sha256_bytes(diff),
        "tracked_diff_bytes": len(diff),
        "untracked": untracked,
        "submodule_status_sha256": _sha256_bytes(submodules),
        "submodule_status": submodules.decode("utf-8", "replace").splitlines(),
    }
    encoded = json.dumps(material, sort_keys=True, separators=(",", ":")).encode()
    material["state_sha256"] = _sha256_bytes(encoded)
    material["clean"] = not diff and not untracked and not submodules.strip()
    return material


def source_identity(root: Optional[Path] = None) -> Dict[str, object]:
    """Describe commit, tree, cleanliness, and all source bytes in one record."""

    source = Path(root) if root is not None else repository_root()
    if source is None:
        source = Path(__file__).resolve().parent
    source = source.resolve()
    state = _source_state(source)
    if state["commit"] == "unborn":
        tree = None
    else:
        tree = _run_bytes(("git", "rev-parse", "HEAD^{tree}"), source).decode().strip()
    return {"repository": REPOSITORY_NAME, "tree": tree, **state}


def source_identity_sha256(identity: Dict[str, object]) -> str:
    payload = json.dumps(identity, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


__all__ = [
    "REPOSITORY_NAME",
    "_source_state",
    "source_identity",
    "source_identity_sha256",
]
