"""Versioned, integrity-checked persistent calibration storage."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile
from contextlib import contextmanager
import fcntl
from typing import Dict, Mapping, Optional

from .errors import FirmwaveError


BUNDLE_SCHEMA = "neptunesdr.calibration-bundle/v1"
INDEX_SCHEMA = "neptunesdr.calibration-index/v1"
CALIBRATION_TYPES = {
    "DC_OFFSET",
    "IQ_IMBALANCE",
    "CHANNEL_AMPLITUDE_PHASE",
    "FREQUENCY_RESPONSE_EQ",
    "TEMPERATURE_COMPENSATION",
}
MAX_COEFFICIENTS = 262_144
SERIAL_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")


class CalibrationError(FirmwaveError):
    """Calibration input or persistent-state validation failed."""


def _canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def bundle_sha256(value: Mapping[str, object]) -> str:
    payload = dict(value)
    payload.pop("integrity_sha256", None)
    return hashlib.sha256(_canonical(payload)).hexdigest()


def _integer(value: object, label: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise CalibrationError("%s must be an integer in [%d, %d]" % (label, minimum, maximum))
    return value


def validate_bundle(value: Mapping[str, object], *, expected_serial: Optional[str] = None) -> Dict[str, object]:
    if not isinstance(value, dict) or value.get("schema") != BUNDLE_SCHEMA:
        raise CalibrationError("unsupported calibration bundle schema")
    serial = value.get("device_serial")
    if not isinstance(serial, str) or SERIAL_RE.fullmatch(serial) is None:
        raise CalibrationError("invalid calibration device serial")
    if expected_serial is not None and serial != expected_serial:
        raise CalibrationError("calibration bundle belongs to a different device")
    revision = _integer(value.get("revision"), "revision", 0, 0xFFFFFFFF)
    previous_revision = value.get("previous_revision")
    if revision == 0:
        if previous_revision is not None:
            raise CalibrationError("factory revision zero cannot name a predecessor")
    else:
        _integer(previous_revision, "previous_revision", 0, revision - 1)
    tables = value.get("tables")
    if not isinstance(tables, list) or not tables:
        raise CalibrationError("calibration bundle must contain at least one table")
    total_coefficients = 0
    identities = set()
    for index, table in enumerate(tables):
        label = "tables[%d]" % index
        if not isinstance(table, dict) or table.get("calibration_type") not in CALIBRATION_TYPES:
            raise CalibrationError("%s has unsupported calibration_type" % label)
        channel_mask = _integer(table.get("channel_mask"), label + ".channel_mask", 1, 3)
        frequency_min = _integer(table.get("frequency_min_hz"), label + ".frequency_min_hz", 70_000_000, 6_000_000_000)
        frequency_max = _integer(table.get("frequency_max_hz"), label + ".frequency_max_hz", frequency_min, 6_000_000_000)
        sample_rate = _integer(table.get("sample_rate_hz"), label + ".sample_rate_hz", 1, 61_440_000)
        bandwidth = _integer(table.get("bandwidth_hz"), label + ".bandwidth_hz", 200_000, 56_000_000)
        gain_min = _integer(table.get("gain_min_mdb"), label + ".gain_min_mdb", -100_000, 100_000)
        gain_max = _integer(table.get("gain_max_mdb"), label + ".gain_max_mdb", gain_min, 100_000)
        temperature_min = _integer(table.get("temperature_min_mc"), label + ".temperature_min_mc", -40_000, 125_000)
        temperature_max = _integer(table.get("temperature_max_mc"), label + ".temperature_max_mc", temperature_min, 125_000)
        coefficients = table.get("coefficients_q2_30")
        if not isinstance(coefficients, list) or not coefficients:
            raise CalibrationError("%s needs coefficients_q2_30" % label)
        for coefficient in coefficients:
            _integer(coefficient, label + ".coefficient", -(1 << 31), (1 << 31) - 1)
        total_coefficients += len(coefficients)
        # This is the canonical table key documented by the bundle schema.
        # Bundle serial/revision are included explicitly even though they are
        # constant inside this loop, preventing future cross-bundle indexing
        # code from silently using a weaker identity.
        identity = (
            serial,
            channel_mask,
            frequency_min,
            frequency_max,
            sample_rate,
            bandwidth,
            gain_min,
            gain_max,
            temperature_min,
            temperature_max,
            table["calibration_type"],
            revision,
        )
        if identity in identities:
            raise CalibrationError("duplicate calibration table key in bundle")
        identities.add(identity)
    if total_coefficients > MAX_COEFFICIENTS:
        raise CalibrationError("calibration coefficient count exceeds safety limit")
    observed = value.get("integrity_sha256")
    expected = bundle_sha256(value)
    if not isinstance(observed, str) or observed != expected:
        raise CalibrationError("calibration bundle integrity SHA-256 mismatch")
    # JSON round trip rejects custom mapping subclasses and guarantees a plain,
    # immutable-on-disk representation for subsequent validation.
    return json.loads(_canonical(value).decode("ascii"))


class CalibrationStore:
    def __init__(self, root: Path, device_serial: str):
        if SERIAL_RE.fullmatch(device_serial) is None:
            raise CalibrationError("invalid store device serial")
        self.root = Path(root)
        self.device_serial = device_serial

    @property
    def index_path(self) -> Path:
        return self.root / "index.json"

    def _bundle_path(self, revision: int) -> Path:
        return self.root / ("revision-%010d.json" % revision)

    @contextmanager
    def _locked(self, *, exclusive: bool):
        self.root.mkdir(mode=0o750, parents=True, exist_ok=True)
        if self.root.is_symlink() or not self.root.is_dir():
            raise CalibrationError("refusing unsafe calibration root")
        flags = os.O_RDWR | os.O_CREAT
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        try:
            descriptor = os.open(str(self.root / ".store.lock"), flags, 0o640)
        except OSError as exc:
            raise CalibrationError("cannot open calibration store lock: %s" % exc) from exc
        try:
            if not stat.S_ISREG(os.fstat(descriptor).st_mode):
                raise CalibrationError("calibration store lock is not a regular file")
            fcntl.flock(descriptor, fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH)
            yield
        finally:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_UN)
            finally:
                os.close(descriptor)

    def _fsync_root(self) -> None:
        flags = os.O_RDONLY
        if hasattr(os, "O_DIRECTORY"):
            flags |= os.O_DIRECTORY
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        descriptor = os.open(str(self.root), flags)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def _atomic_write(self, path: Path, value: Mapping[str, object]) -> None:
        self.root.mkdir(mode=0o750, parents=True, exist_ok=True)
        if path.is_symlink():
            raise CalibrationError("refusing calibration symlink target")
        encoded = json.dumps(value, indent=2, sort_keys=True).encode("utf-8") + b"\n"
        descriptor, temporary_name = tempfile.mkstemp(prefix=".calibration-", dir=str(self.root))
        temporary = Path(temporary_name)
        try:
            os.fchmod(descriptor, 0o640)
            with os.fdopen(descriptor, "wb") as output:
                output.write(encoded)
                output.flush()
                os.fsync(output.fileno())
            os.replace(str(temporary), str(path))
            self._fsync_root()
        finally:
            if temporary.exists():
                temporary.unlink()

    def _read_json(self, path: Path) -> Mapping[str, object]:
        if path.is_symlink() or not path.is_file():
            raise CalibrationError("missing or unsafe calibration state: %s" % path)
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise CalibrationError("cannot read calibration state: %s" % exc) from exc
        if not isinstance(value, dict):
            raise CalibrationError("calibration state must be a JSON object")
        return value

    def _active_index_unlocked(self) -> Optional[Mapping[str, object]]:
        if not self.index_path.exists():
            return None
        value = self._read_json(self.index_path)
        if value.get("schema") != INDEX_SCHEMA or value.get("device_serial") != self.device_serial:
            raise CalibrationError("invalid calibration index identity")
        revision = _integer(value.get("active_revision"), "active_revision", 0, 0xFFFFFFFF)
        bundle = validate_bundle(self._read_json(self._bundle_path(revision)), expected_serial=self.device_serial)
        if value.get("bundle_sha256") != bundle["integrity_sha256"]:
            raise CalibrationError("calibration index does not match active bundle")
        _integer(value.get("activation_timestamp"), "activation_timestamp", 0, (1 << 64) - 1)
        return value

    def active_index(self) -> Optional[Mapping[str, object]]:
        with self._locked(exclusive=False):
            return self._active_index_unlocked()

    def install(self, value: Mapping[str, object], *, activation_timestamp: int) -> Mapping[str, object]:
        bundle = validate_bundle(value, expected_serial=self.device_serial)
        activation_timestamp = _integer(activation_timestamp, "activation_timestamp", 0, (1 << 64) - 1)
        with self._locked(exclusive=True):
            active = self._active_index_unlocked()
            revision = int(bundle["revision"])
            if active is None:
                if revision != 0:
                    raise CalibrationError("first installed calibration must be factory revision zero")
            else:
                active_revision = int(active["active_revision"])
                if revision <= active_revision or bundle["previous_revision"] != active_revision:
                    raise CalibrationError("calibration revision chain does not extend active state")
            target = self._bundle_path(revision)
            if target.is_symlink():
                raise CalibrationError("refusing calibration symlink target")
            if target.exists():
                # A crash may occur after the immutable revision was fsynced but
                # before the active index was replaced.  Only an exact validated
                # orphan may be reconciled by retrying the same transaction.
                orphan = validate_bundle(
                    self._read_json(target), expected_serial=self.device_serial
                )
                if orphan["integrity_sha256"] != bundle["integrity_sha256"]:
                    raise CalibrationError("calibration revision already exists with different content")
            else:
                self._atomic_write(target, bundle)
            index = {
                "schema": INDEX_SCHEMA,
                "device_serial": self.device_serial,
                "active_revision": revision,
                "bundle_sha256": bundle["integrity_sha256"],
                "activation_timestamp": activation_timestamp,
            }
            self._atomic_write(self.index_path, index)
            return index

    def rollback(self, revision: int, *, activation_timestamp: int) -> Mapping[str, object]:
        revision = _integer(revision, "revision", 0, 0xFFFFFFFF)
        activation_timestamp = _integer(activation_timestamp, "activation_timestamp", 0, (1 << 64) - 1)
        with self._locked(exclusive=True):
            bundle = validate_bundle(
                self._read_json(self._bundle_path(revision)),
                expected_serial=self.device_serial,
            )
            index = {
                "schema": INDEX_SCHEMA,
                "device_serial": self.device_serial,
                "active_revision": revision,
                "bundle_sha256": bundle["integrity_sha256"],
                "activation_timestamp": activation_timestamp,
            }
            self._atomic_write(self.index_path, index)
            return index


__all__ = [
    "BUNDLE_SCHEMA",
    "CalibrationError",
    "CalibrationStore",
    "bundle_sha256",
    "validate_bundle",
]
