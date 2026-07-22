"""Signed, rollback-resistant SD update-manifest verification."""

from __future__ import annotations

import base64
import binascii
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import stat
import tempfile
from typing import Dict, Mapping, Tuple

from .errors import FirmwaveError


MANIFEST_SCHEMA = "neptunesdr.update-manifest/v1"
PLATFORM_ID = "p210-xc7z020-custom-v0"
REQUIRED_ROLES = {"boot", "rootfs"}
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


class UpdateManifestError(FirmwaveError):
    """An update manifest, signature, or payload failed closed."""


def canonical_signing_bytes(value: Mapping[str, object]) -> bytes:
    payload = dict(value)
    payload.pop("signature_base64", None)
    return json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def _sha256_fd(descriptor: int) -> str:
    digest = hashlib.sha256()
    os.lseek(descriptor, 0, os.SEEK_SET)
    while True:
        chunk = os.read(descriptor, 1024 * 1024)
        if not chunk:
            break
        digest.update(chunk)
    return digest.hexdigest()


def _open_flags(*, directory: bool) -> int:
    if not hasattr(os, "O_NOFOLLOW"):
        raise UpdateManifestError("platform cannot enforce no-follow update admission")
    flags = os.O_RDONLY | os.O_NOFOLLOW
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NONBLOCK"):
        flags |= os.O_NONBLOCK
    if directory:
        if not hasattr(os, "O_DIRECTORY"):
            raise UpdateManifestError("platform cannot enforce directory update admission")
        flags |= os.O_DIRECTORY
    return flags


def _stat_snapshot(observed: os.stat_result) -> Tuple[int, ...]:
    return (
        observed.st_dev,
        observed.st_ino,
        observed.st_mode,
        observed.st_size,
        observed.st_mtime_ns,
        observed.st_ctime_ns,
    )


def _open_artifact_root(path: Path) -> int:
    absolute = Path(os.path.abspath(str(path)))
    try:
        parent = os.open(str(absolute.parent), _open_flags(directory=True))
    except OSError as exc:
        raise UpdateManifestError("update artifact root parent is missing or unsafe") from exc
    try:
        try:
            descriptor = os.open(
                absolute.name,
                _open_flags(directory=True),
                dir_fd=parent,
            )
        except OSError as exc:
            raise UpdateManifestError("update artifact root is missing or unsafe") from exc
    finally:
        os.close(parent)
    if not stat.S_ISDIR(os.fstat(descriptor).st_mode):
        os.close(descriptor)
        raise UpdateManifestError("update artifact root is not a directory")
    return descriptor


def _open_relative_artifact(root_descriptor: int, relative: str) -> int:
    parts = PurePosixPath(relative).parts
    parent = os.dup(root_descriptor)
    try:
        for component in parts[:-1]:
            try:
                child = os.open(
                    component,
                    _open_flags(directory=True),
                    dir_fd=parent,
                )
            except OSError as exc:
                raise UpdateManifestError(
                    "unsafe update artifact parent for %s" % relative
                ) from exc
            os.close(parent)
            parent = child
        try:
            descriptor = os.open(
                parts[-1],
                _open_flags(directory=False),
                dir_fd=parent,
            )
        except OSError as exc:
            raise UpdateManifestError("missing or unsafe update artifact %s" % relative) from exc
    finally:
        os.close(parent)
    observed = os.fstat(descriptor)
    if not stat.S_ISREG(observed.st_mode):
        os.close(descriptor)
        raise UpdateManifestError("update artifact is not a regular file: %s" % relative)
    return descriptor


def _snapshot_regular_file(path: Path, label: str, *, maximum_bytes: int) -> bytes:
    try:
        parent = os.open(str(path.parent), _open_flags(directory=True))
    except OSError as exc:
        raise UpdateManifestError("%s parent is missing or unsafe" % label) from exc
    try:
        try:
            descriptor = os.open(
                path.name,
                _open_flags(directory=False),
                dir_fd=parent,
            )
        except OSError as exc:
            raise UpdateManifestError("%s is missing or unsafe" % label) from exc
    finally:
        os.close(parent)
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or before.st_size > maximum_bytes:
            raise UpdateManifestError("%s is not a bounded regular file" % label)
        os.lseek(descriptor, 0, os.SEEK_SET)
        chunks = []
        remaining = before.st_size
        while remaining:
            chunk = os.read(descriptor, min(remaining, 64 * 1024))
            if not chunk:
                raise UpdateManifestError("%s changed while being read" % label)
            chunks.append(chunk)
            remaining -= len(chunk)
        after = os.fstat(descriptor)
        if _stat_snapshot(before) != _stat_snapshot(after):
            raise UpdateManifestError("%s changed during admission" % label)
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def _safe_relative(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or "\\" in value:
        raise UpdateManifestError("%s must be a POSIX relative path" % label)
    path = PurePosixPath(value)
    canonical = path.as_posix()
    if path.is_absolute() or canonical in (".", "..") or ".." in path.parts:
        raise UpdateManifestError("%s escapes the update root" % label)
    return canonical


def validate_update_manifest(
    value: Mapping[str, object],
    artifact_root: Path,
    *,
    public_key: Path,
    accepted_rollback_index: int,
    openssl: str = "openssl",
) -> Dict[str, object]:
    if not isinstance(value, dict) or value.get("schema") != MANIFEST_SCHEMA:
        raise UpdateManifestError("unsupported update manifest schema")
    if value.get("platform_id") != PLATFORM_ID:
        raise UpdateManifestError("update targets a different hardware platform")
    if value.get("target_media") != "removable-sd" or value.get("target_slot") not in ("A", "B"):
        raise UpdateManifestError("updates are restricted to removable SD A/B slots")
    if value.get("qspi_update_allowed") is not False:
        raise UpdateManifestError("update manifest must explicitly forbid QSPI writes")
    rollback_index = value.get("rollback_index")
    if (
        isinstance(rollback_index, bool)
        or not isinstance(rollback_index, int)
        or not 0 <= rollback_index <= 0xFFFFFFFFFFFFFFFF
        or rollback_index <= accepted_rollback_index
    ):
        raise UpdateManifestError("update rollback index does not advance device state")
    key_id = value.get("signing_key_id")
    if not isinstance(key_id, str) or re.fullmatch(r"[A-Za-z0-9._-]{1,64}", key_id) is None:
        raise UpdateManifestError("invalid signing key identifier")
    artifacts = value.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        raise UpdateManifestError("update manifest has no artifacts")
    observed_paths = set()
    observed_roles = set()
    observed_files = set()
    root_descriptor = _open_artifact_root(Path(artifact_root))
    try:
        for index, artifact in enumerate(artifacts):
            label = "artifacts[%d]" % index
            if not isinstance(artifact, dict):
                raise UpdateManifestError("%s must be an object" % label)
            relative = _safe_relative(artifact.get("path"), label + ".path")
            if relative in observed_paths:
                raise UpdateManifestError("duplicate update artifact path")
            role = artifact.get("role")
            if not isinstance(role, str) or re.fullmatch(r"[a-z][a-z0-9_-]{0,31}", role) is None:
                raise UpdateManifestError("%s has invalid role" % label)
            if role in observed_roles:
                raise UpdateManifestError("duplicate update artifact role")
            size = artifact.get("bytes")
            digest = artifact.get("sha256")
            if isinstance(size, bool) or not isinstance(size, int) or size < 0:
                raise UpdateManifestError("%s has invalid byte count" % label)
            if not isinstance(digest, str) or HEX64.fullmatch(digest) is None:
                raise UpdateManifestError("%s has invalid SHA-256" % label)
            descriptor = _open_relative_artifact(root_descriptor, relative)
            try:
                before = os.fstat(descriptor)
                file_identity = (before.st_dev, before.st_ino)
                if file_identity in observed_files:
                    raise UpdateManifestError("update artifact paths alias the same file")
                observed_digest = _sha256_fd(descriptor)
                after = os.fstat(descriptor)
                if _stat_snapshot(before) != _stat_snapshot(after):
                    raise UpdateManifestError(
                        "update artifact changed during admission: %s" % relative
                    )
                if before.st_size != size or observed_digest != digest:
                    raise UpdateManifestError("update artifact identity mismatch for %s" % relative)
            finally:
                os.close(descriptor)
            observed_paths.add(relative)
            observed_roles.add(role)
            observed_files.add(file_identity)
    finally:
        os.close(root_descriptor)
    if not REQUIRED_ROLES.issubset(observed_roles):
        raise UpdateManifestError("update omits required boot/rootfs artifacts")

    signature_value = value.get("signature_base64")
    if not isinstance(signature_value, str):
        raise UpdateManifestError("update signature is missing")
    try:
        signature = base64.b64decode(signature_value, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise UpdateManifestError("update signature is not strict base64") from exc
    if len(signature) != 64:
        raise UpdateManifestError("Ed25519 signature must be 64 bytes")
    public_key_bytes = _snapshot_regular_file(
        Path(public_key), "trusted update public key", maximum_bytes=64 * 1024
    )

    with tempfile.TemporaryDirectory(prefix="neptune-update-verify-") as temporary:
        temporary_path = Path(temporary)
        payload_path = temporary_path / "manifest.canonical.json"
        signature_path = temporary_path / "manifest.signature"
        public_key_path = temporary_path / "trusted-public-key.pem"
        payload_path.write_bytes(canonical_signing_bytes(value))
        signature_path.write_bytes(signature)
        public_key_path.write_bytes(public_key_bytes)
        try:
            result = subprocess.run(
                [
                    openssl,
                    "pkeyutl",
                    "-verify",
                    "-pubin",
                    "-inkey",
                    str(public_key_path),
                    "-rawin",
                    "-in",
                    str(payload_path),
                    "-sigfile",
                    str(signature_path),
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env={"PATH": os.environ.get("PATH", ""), "LC_ALL": "C"},
            )
        except OSError as exc:
            raise UpdateManifestError("cannot execute trusted signature verifier") from exc
    if result.returncode != 0:
        raise UpdateManifestError("update Ed25519 signature verification failed")
    return json.loads(canonical_signing_bytes(value).decode("ascii")) | {
        "signature_base64": signature_value
    }


__all__ = [
    "MANIFEST_SCHEMA",
    "PLATFORM_ID",
    "UpdateManifestError",
    "canonical_signing_bytes",
    "validate_update_manifest",
]
