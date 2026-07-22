"""Fail-closed validation for checked-in firmware and runtime locks."""

from __future__ import annotations

import json
from pathlib import Path
import re
from typing import Dict, Mapping, Optional
from urllib.parse import urlparse

from .errors import FirmwareFormatError


def data_path(name: str) -> Path:
    path = Path(__file__).with_name("data") / name
    if not path.is_file():
        raise FirmwareFormatError("packaged lock is missing: %s" % path)
    return path


def validate_firmware_lock(path: Optional[Path] = None) -> Mapping[str, object]:
    source = Path(path) if path is not None else data_path("firmware-lock.json")
    try:
        lock = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FirmwareFormatError("cannot read firmware lock %s: %s" % (source, exc)) from exc
    if not isinstance(lock, dict) or lock.get("schema") != 1:
        raise FirmwareFormatError("firmware lock must use schema 1")
    artifacts = lock.get("artifacts")
    if not isinstance(artifacts, dict) or not artifacts:
        raise FirmwareFormatError("firmware lock has no artifacts")
    digests = set()
    for name, entry in sorted(artifacts.items()):
        if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9][a-z0-9.-]*", name):
            raise FirmwareFormatError("invalid artifact name %r" % name)
        if not isinstance(entry, dict):
            raise FirmwareFormatError("artifact %s is not an object" % name)
        url = entry.get("url")
        digest = entry.get("sha256")
        size = entry.get("bytes")
        if not isinstance(url, str) or urlparse(url).scheme != "https":
            raise FirmwareFormatError("artifact %s must use an HTTPS URL" % name)
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise FirmwareFormatError("artifact %s has an invalid SHA-256" % name)
        if digest in digests:
            raise FirmwareFormatError("artifact %s reuses another artifact digest" % name)
        digests.add(digest)
        if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
            raise FirmwareFormatError("artifact %s has an invalid byte count" % name)
        if not isinstance(entry.get("kind"), str) or not entry["kind"]:
            raise FirmwareFormatError("artifact %s has no kind" % name)
    return lock


def validate_runtime_lock(
    path: Optional[Path] = None,
    firmware_path: Optional[Path] = None,
) -> Mapping[str, object]:
    source = Path(path) if path is not None else data_path("runtime-lock.json")
    try:
        lock = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FirmwareFormatError("cannot read runtime lock %s: %s" % (source, exc)) from exc
    if not isinstance(lock, dict) or lock.get("schema") != 1:
        raise FirmwareFormatError("runtime lock must use schema 1")
    inputs = lock.get("inputs")
    if not isinstance(inputs, dict) or set(inputs) != {"p210-sd-boot", "plutosdr-fw-v0.39"}:
        raise FirmwareFormatError("runtime lock does not name the exact two composed inputs")
    firmware = validate_firmware_lock(firmware_path)
    locked = firmware["artifacts"]
    for name, entry in inputs.items():
        if entry.get("firmware_lock_sha256") != locked[name]["sha256"]:
            raise FirmwareFormatError("runtime input %s does not match firmware-lock.json" % name)
    return lock


def lock_summary(
    firmware_path: Optional[Path] = None,
    runtime_path: Optional[Path] = None,
) -> Dict[str, object]:
    firmware = validate_firmware_lock(firmware_path)
    runtime = validate_runtime_lock(runtime_path, firmware_path)
    return {
        "firmware_schema": firmware["schema"],
        "runtime_schema": runtime["schema"],
        "artifacts": sorted(firmware["artifacts"]),
        "valid": True,
    }


__all__ = [
    "data_path",
    "lock_summary",
    "validate_firmware_lock",
    "validate_runtime_lock",
]
