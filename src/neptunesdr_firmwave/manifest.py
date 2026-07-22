"""Runtime-manifest construction shared by scripts and tests."""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Dict, Iterable, Mapping, MutableMapping, Optional, Tuple

from .interface import INTERFACE_SCHEMA, interface_sha256, load_interface
from .provenance import source_identity


RUNTIME_MANIFEST_SCHEMA = "neptunesdr.firmwave.runtime-manifest/v1"


def file_record(path: Path, output_root: Path, *, role: str) -> Dict[str, object]:
    source = Path(path).resolve()
    root = Path(output_root).resolve()
    try:
        relative = source.relative_to(root)
    except ValueError as exc:
        raise ValueError("generated artifact escapes runtime output: %s" % source) from exc
    if not relative.parts or ".." in relative.parts:
        raise ValueError("invalid generated artifact path: %s" % relative)
    digest = hashlib.sha256()
    size = 0
    with source.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            size += len(chunk)
            digest.update(chunk)
    return {
        "path": relative.as_posix(),
        "bytes": size,
        "sha256": digest.hexdigest(),
        "role": role,
    }


def finish_runtime_manifest(
    manifest: MutableMapping[str, object],
    artifacts: Iterable[Tuple[str, Path, str]],
    *,
    output_root: Path,
    source_root: Optional[Path] = None,
) -> MutableMapping[str, object]:
    """Add the non-flashable profile, source identity, and complete output hashes."""

    interface = load_interface()
    records: Dict[str, Mapping[str, object]] = {}
    for name, path, role in artifacts:
        if name in records:
            raise ValueError("duplicate generated artifact name %r" % name)
        records[name] = file_record(path, output_root, role=role)
    manifest["schema"] = RUNTIME_MANIFEST_SCHEMA
    manifest["profile"] = "qemu-development"
    manifest["flashable"] = False
    manifest["interface"] = {
        "schema": INTERFACE_SCHEMA,
        "path": "specs/p210-firmware-interface-v1.json",
        "sha256": interface_sha256(),
        "abi_version": interface["pl_fft_abi"]["version"],
    }
    manifest["firmwave_source"] = source_identity(source_root)
    manifest["generated_artifacts"] = records
    manifest["artifact_hashes_complete"] = True
    manifest["artifact_scope"] = (
        "all published runtime outputs except this self-describing manifest; "
        ".inputs is reproducible extraction staging"
    )
    return manifest


__all__ = ["RUNTIME_MANIFEST_SCHEMA", "file_record", "finish_runtime_manifest"]
