"""Reproducible identity for the exact Firmware source state."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import hashlib
from typing import Any, Dict, List, Optional, Sequence

from .errors import ProvenanceError
from .interface import repository_root


REPOSITORY_NAME = "Atom-NeptuneSDR-Firmware"


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


def _git_root(root: Path) -> Path:
    try:
        rendered = _run_bytes(("git", "rev-parse", "--show-toplevel"), root)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ProvenanceError(
            "source identity requires a Git checkout; pass --root for the "
            "Atom-NeptuneSDR-Firmware checkout"
        ) from exc
    try:
        source = Path(rendered.decode("utf-8").strip()).resolve(strict=True)
    except (OSError, UnicodeError, ValueError) as exc:
        raise ProvenanceError("Git returned an invalid repository root") from exc
    if not source.is_dir():
        raise ProvenanceError("Git repository root is not a directory: %s" % source)
    return source


def _source_state(root: Path) -> Dict[str, Any]:
    """Match the consuming Twin's acceptance source-state algorithm exactly."""

    root = _git_root(root.resolve())
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
        try:
            commit = "unborn"
            branch = _run_bytes(
                ("git", "symbolic-ref", "--short", "HEAD"), root
            ).decode().strip()
            diff = b""
        except (OSError, subprocess.CalledProcessError, UnicodeError) as exc:
            raise ProvenanceError("cannot identify the Git source state") from exc
    try:
        untracked_raw = _run_bytes(
            ("git", "ls-files", "--others", "--exclude-standard", "-z"), root
        )
        index_tags_raw = _run_bytes(("git", "ls-files", "-v", "-z"), root)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ProvenanceError("cannot enumerate the Git source index") from exc
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
    hidden_index_flags: List[Dict[str, str]] = []
    for encoded in index_tags_raw.split(b"\0"):
        if not encoded:
            continue
        if len(encoded) < 3 or encoded[1:2] != b" ":
            raise ProvenanceError("Git returned a malformed tracked-file record")
        tag = chr(encoded[0])
        if tag == "S" or tag.islower():
            hidden_index_flags.append(
                {"path": os.fsdecode(encoded[2:]), "tag": tag}
            )
    try:
        submodules = _run_bytes(("git", "submodule", "status", "--recursive"), root)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ProvenanceError("cannot identify Git submodule state") from exc
    material: Dict[str, Any] = {
        "commit": commit,
        "branch": branch,
        "tracked_diff_sha256": _sha256_bytes(diff),
        "tracked_diff_bytes": len(diff),
        "hidden_index_flags": hidden_index_flags,
        "untracked": untracked,
        "submodule_status_sha256": _sha256_bytes(submodules),
        "submodule_status": submodules.decode("utf-8", "replace").splitlines(),
    }
    encoded = json.dumps(material, sort_keys=True, separators=(",", ":")).encode()
    material["state_sha256"] = _sha256_bytes(encoded)
    material["clean"] = (
        not diff
        and not hidden_index_flags
        and not untracked
        and not submodules.strip()
    )
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
        try:
            tree = _run_bytes(
                ("git", "rev-parse", "HEAD^{tree}"), source
            ).decode().strip()
        except (OSError, subprocess.CalledProcessError, UnicodeError) as exc:
            raise ProvenanceError("cannot identify the Git source tree") from exc
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
