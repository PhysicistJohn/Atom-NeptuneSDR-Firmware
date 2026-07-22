"""Fail-closed P210 hardware-evidence manifest validation.

The manifest records observations from independently sourced artifacts.  This
module validates the document structure, binds each observation to the
firmware lock, and computes compatibility from cross-artifact agreement.  A
well-formed manifest can therefore be incompatible without being malformed.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import re
import sysconfig
from typing import Dict, List, Mapping, Optional, Sequence, Tuple

from .errors import HardwareManifestError
from .locks import data_path, validate_firmware_lock


HARDWARE_MANIFEST_SCHEMA = "neptunesdr.p210-hardware-manifest/v1"
HARDWARE_MANIFEST_NAME = "p210-hardware-candidate-v1.json"
HARDWARE_SCHEMA_NAME = "p210-hardware-manifest-v1.schema.json"

P210_REQUIRED_GPIO_CONTROLS = (
    "ad9361_en_agc",
    "ad9361_sync",
    "ad9361_reset",
    "ad9361_enable",
    "ad9361_txnrx",
    "usb0_reset",
)

REQUIRED_CONTACTS = (
    "part",
    "ddr.bus_width_bits",
    "ddr.size_bytes",
    "radio.digital_interface",
    "radio.rx_channels",
    "radio.tx_channels",
    "gpio.mio_count",
    "gpio.emio_width",
    "build.vivado_version",
    "build.source_identity",
)

_CONTACT_PATHS = {
    "part": ("part",),
    "ddr.bus_width_bits": ("ddr", "bus_width_bits"),
    "ddr.size_bytes": ("ddr", "size_bytes"),
    "radio.digital_interface": ("radio", "digital_interface"),
    "radio.rx_channels": ("radio", "rx_channels"),
    "radio.tx_channels": ("radio", "tx_channels"),
    "gpio.mio_count": ("gpio", "mio_count"),
    "gpio.emio_width": ("gpio", "emio_width"),
    "build.vivado_version": ("build", "vivado_version"),
    "build.source_identity": ("build", "source_identity"),
}


@dataclass(frozen=True)
class HardwareManifestIssue:
    severity: str
    check: str
    message: str
    artifacts: Tuple[str, ...] = ()
    observed: Mapping[str, object] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, object]:
        return {
            "severity": self.severity,
            "check": self.check,
            "message": self.message,
            "artifacts": list(self.artifacts),
            "observed": dict(self.observed),
        }


@dataclass
class HardwareManifestReport:
    source: str
    candidate_id: str
    artifact_ids: Tuple[str, ...]
    consensus: Dict[str, object] = field(default_factory=dict)
    issues: List[HardwareManifestIssue] = field(default_factory=list)

    @property
    def compatible(self) -> bool:
        return not any(issue.severity == "error" for issue in self.issues)

    def add(
        self,
        check: str,
        message: str,
        artifacts: Sequence[str] = (),
        observed: Optional[Mapping[str, object]] = None,
    ) -> None:
        self.issues.append(
            HardwareManifestIssue(
                severity="error",
                check=check,
                message=message,
                artifacts=tuple(artifacts),
                observed=dict(observed or {}),
            )
        )

    def to_dict(self) -> Dict[str, object]:
        return {
            "schema": HARDWARE_MANIFEST_SCHEMA,
            "source": self.source,
            "candidate_id": self.candidate_id,
            "compatible": self.compatible,
            "artifact_ids": list(self.artifact_ids),
            "consensus": dict(self.consensus),
            "issues": [issue.to_dict() for issue in self.issues],
        }


def _repository_root() -> Optional[Path]:
    candidate = Path(__file__).resolve().parents[2]
    if (candidate / "pyproject.toml").is_file():
        return candidate
    return None


def hardware_manifest_path(explicit: Optional[Path] = None) -> Path:
    if explicit is not None:
        candidate = Path(explicit)
    elif os.environ.get("NEPTUNE_FIRMWAVE_HARDWARE_MANIFEST"):
        candidate = Path(os.environ["NEPTUNE_FIRMWAVE_HARDWARE_MANIFEST"])
    else:
        candidate = data_path(HARDWARE_MANIFEST_NAME)
    if not candidate.is_file():
        raise HardwareManifestError("hardware manifest is missing: %s" % candidate)
    return candidate.resolve()


def hardware_schema_path(explicit: Optional[Path] = None) -> Path:
    if explicit is not None:
        candidate = Path(explicit)
    else:
        root = _repository_root()
        if root is not None:
            candidate = root / "specs" / HARDWARE_SCHEMA_NAME
        else:
            candidate = (
                Path(sysconfig.get_path("data"))
                / "share"
                / "neptunesdr-firmwave"
                / "specs"
                / HARDWARE_SCHEMA_NAME
            )
    if not candidate.is_file():
        raise HardwareManifestError("hardware manifest schema is missing: %s" % candidate)
    return candidate.resolve()


def _object(value: object, label: str) -> Mapping[str, object]:
    if not isinstance(value, dict):
        raise HardwareManifestError("%s must be an object" % label)
    return value


def _exact_keys(
    value: Mapping[str, object],
    required: Sequence[str],
    label: str,
) -> None:
    expected = set(required)
    actual = set(value)
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    if missing or extra:
        details = []
        if missing:
            details.append("missing " + ", ".join(missing))
        if extra:
            details.append("unexpected " + ", ".join(extra))
        raise HardwareManifestError("%s has %s" % (label, "; ".join(details)))


def _identifier(value: object, label: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[a-z0-9][a-z0-9.-]*", value):
        raise HardwareManifestError("%s is not a stable lowercase identifier" % label)
    return value


def _nullable_int(
    value: object,
    label: str,
    *,
    minimum: int = 0,
    maximum: Optional[int] = None,
) -> Optional[int]:
    if value is None:
        return None
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise HardwareManifestError("%s is not an integer in range" % label)
    if maximum is not None and value > maximum:
        raise HardwareManifestError("%s is not an integer in range" % label)
    return value


def _required_int(
    value: object,
    label: str,
    *,
    minimum: int = 0,
    maximum: Optional[int] = None,
) -> int:
    result = _nullable_int(value, label, minimum=minimum, maximum=maximum)
    if result is None:
        raise HardwareManifestError("%s is not an integer in range" % label)
    return result


def _nullable_string(value: object, label: str, pattern: str) -> Optional[str]:
    if value is None:
        return None
    if not isinstance(value, str) or not re.fullmatch(pattern, value):
        raise HardwareManifestError("%s has an invalid value" % label)
    return value


def _validate_source_identity(value: object, label: str) -> None:
    if value is None:
        return
    identity = _object(value, label)
    _exact_keys(identity, ("repository", "revision", "path"), label)
    repository = identity["repository"]
    if not isinstance(repository, str) or not repository.startswith("https://"):
        raise HardwareManifestError("%s.repository must be an HTTPS URL" % label)
    for key in ("revision", "path"):
        item = identity[key]
        if not isinstance(item, str) or not item or any(char in item for char in "\r\n\0"):
            raise HardwareManifestError("%s.%s must be a non-empty scalar" % (label, key))


def _validate_artifact(value: object, index: int) -> Mapping[str, object]:
    label = "artifacts[%d]" % index
    artifact = _object(value, label)
    _exact_keys(artifact, ("id", "role", "source", "contacts"), label)
    _identifier(artifact["id"], label + ".id")
    if artifact["role"] not in ("boot-handoff", "fpga-handoff", "devicetree"):
        raise HardwareManifestError("%s.role is unsupported" % label)

    source = _object(artifact["source"], label + ".source")
    _exact_keys(source, ("lock_artifact", "sha256", "member", "evidence"), label + ".source")
    _identifier(source["lock_artifact"], label + ".source.lock_artifact")
    if not isinstance(source["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", source["sha256"]):
        raise HardwareManifestError("%s.source.sha256 is invalid" % label)
    for key in ("member", "evidence"):
        item = source[key]
        if not isinstance(item, str) or not item or any(char in item for char in "\r\n\0"):
            raise HardwareManifestError("%s.source.%s must be a non-empty scalar" % (label, key))

    contacts = _object(artifact["contacts"], label + ".contacts")
    _exact_keys(contacts, ("part", "ddr", "radio", "gpio", "build"), label + ".contacts")
    _nullable_string(contacts["part"], label + ".contacts.part", r"xc7z[0-9a-z-]+")

    ddr = _object(contacts["ddr"], label + ".contacts.ddr")
    _exact_keys(ddr, ("bus_width_bits", "size_bytes"), label + ".contacts.ddr")
    width = _nullable_int(ddr["bus_width_bits"], label + ".contacts.ddr.bus_width_bits")
    if width is not None and width not in (16, 32):
        raise HardwareManifestError("%s.contacts.ddr.bus_width_bits must be 16 or 32" % label)
    size = _nullable_int(ddr["size_bytes"], label + ".contacts.ddr.size_bytes", minimum=1)
    if size is not None and size % (1024 * 1024):
        raise HardwareManifestError("%s.contacts.ddr.size_bytes must be MiB-aligned" % label)

    radio = _object(contacts["radio"], label + ".contacts.radio")
    _exact_keys(radio, ("digital_interface", "rx_channels", "tx_channels"), label + ".contacts.radio")
    interface = radio["digital_interface"]
    if interface is not None and interface not in ("cmos", "lvds"):
        raise HardwareManifestError("%s.contacts.radio.digital_interface is unsupported" % label)
    for key in ("rx_channels", "tx_channels"):
        channels = _nullable_int(radio[key], "%s.contacts.radio.%s" % (label, key))
        if channels is not None and channels not in (1, 2):
            raise HardwareManifestError("%s.contacts.radio.%s must be one or two" % (label, key))

    gpio = _object(contacts["gpio"], label + ".contacts.gpio")
    _exact_keys(gpio, ("mio_count", "emio_width", "control_offsets"), label + ".contacts.gpio")
    _nullable_int(gpio["mio_count"], label + ".contacts.gpio.mio_count", maximum=256)
    _nullable_int(gpio["emio_width"], label + ".contacts.gpio.emio_width", maximum=256)
    offsets = _object(gpio["control_offsets"], label + ".contacts.gpio.control_offsets")
    for name, offset in offsets.items():
        if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9][a-z0-9._-]*", name):
            raise HardwareManifestError(
                "%s.contacts.gpio.control_offsets has an invalid control name" % label
            )
        _required_int(
            offset,
            "%s.contacts.gpio.control_offsets.%s" % (label, name),
            maximum=4095,
        )

    build = _object(contacts["build"], label + ".contacts.build")
    _exact_keys(build, ("vivado_version", "source_identity"), label + ".contacts.build")
    _nullable_string(build["vivado_version"], label + ".contacts.build.vivado_version", r"[0-9]{4}\.[0-9]+")
    _validate_source_identity(build["source_identity"], label + ".contacts.build.source_identity")
    return artifact


def _validate_structure(value: object) -> Mapping[str, object]:
    manifest = _object(value, "hardware manifest")
    _exact_keys(
        manifest,
        ("schema", "profile", "flashable", "candidate_id", "description", "requirements", "artifacts"),
        "hardware manifest",
    )
    if manifest["schema"] != HARDWARE_MANIFEST_SCHEMA:
        raise HardwareManifestError("unsupported hardware manifest schema %r" % manifest["schema"])
    if manifest["profile"] != "hardware-evidence" or manifest["flashable"] is not False:
        raise HardwareManifestError("hardware manifest must be non-flashable hardware-evidence")
    _identifier(manifest["candidate_id"], "hardware manifest candidate_id")
    if not isinstance(manifest["description"], str) or not manifest["description"]:
        raise HardwareManifestError("hardware manifest description is empty")

    raw_artifacts = manifest["artifacts"]
    if not isinstance(raw_artifacts, list) or len(raw_artifacts) < 2:
        raise HardwareManifestError("hardware manifest requires at least two artifacts")
    artifacts = [_validate_artifact(item, index) for index, item in enumerate(raw_artifacts)]
    artifact_ids = [str(item["id"]) for item in artifacts]
    if len(artifact_ids) != len(set(artifact_ids)):
        raise HardwareManifestError("hardware manifest artifact IDs are not unique")

    requirements = _object(manifest["requirements"], "hardware manifest requirements")
    _exact_keys(
        requirements,
        ("required_artifacts", "minimum_evidence", "required_gpio_controls"),
        "hardware manifest requirements",
    )
    required_artifacts = requirements["required_artifacts"]
    if not isinstance(required_artifacts, list) or len(required_artifacts) < 2:
        raise HardwareManifestError("requirements.required_artifacts needs at least two IDs")
    for index, artifact_id in enumerate(required_artifacts):
        _identifier(artifact_id, "requirements.required_artifacts[%d]" % index)
    if len(required_artifacts) != len(set(required_artifacts)):
        raise HardwareManifestError("requirements.required_artifacts contains duplicates")
    unknown = sorted(set(required_artifacts) - set(artifact_ids))
    if unknown:
        raise HardwareManifestError("required artifact IDs are undefined: %s" % ", ".join(unknown))

    minimum = _object(requirements["minimum_evidence"], "requirements.minimum_evidence")
    _exact_keys(minimum, REQUIRED_CONTACTS, "requirements.minimum_evidence")
    for contact, count in minimum.items():
        if (
            not isinstance(count, int)
            or isinstance(count, bool)
            or count < 2
            or count > len(required_artifacts)
        ):
            raise HardwareManifestError(
                "requirements.minimum_evidence.%s is outside the required-artifact count" % contact
            )

    controls = requirements["required_gpio_controls"]
    if not isinstance(controls, list) or not all(isinstance(item, str) for item in controls):
        raise HardwareManifestError(
            "requirements.required_gpio_controls must name the complete P210 control set"
        )
    if set(controls) != set(P210_REQUIRED_GPIO_CONTROLS):
        raise HardwareManifestError(
            "requirements.required_gpio_controls must name the complete P210 control set"
        )
    if len(controls) != len(set(controls)):
        raise HardwareManifestError("requirements.required_gpio_controls contains duplicates")
    return manifest


def load_hardware_manifest(path: Optional[Path] = None) -> Mapping[str, object]:
    source = hardware_manifest_path(path)
    try:
        value = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HardwareManifestError("cannot read hardware manifest %s: %s" % (source, exc)) from exc
    return _validate_structure(value)


def _contact_value(contacts: Mapping[str, object], path: Sequence[str]) -> object:
    value: object = contacts
    for component in path:
        value = value[component]  # type: ignore[index]
    return value


def _distinct(values: Mapping[str, object]) -> int:
    return len(
        {
            json.dumps(value, sort_keys=True, separators=(",", ":"))
            for value in values.values()
        }
    )


def validate_hardware_manifest(
    path: Optional[Path] = None,
    firmware_lock_path: Optional[Path] = None,
) -> HardwareManifestReport:
    source = hardware_manifest_path(path)
    manifest = load_hardware_manifest(source)
    firmware = validate_firmware_lock(firmware_lock_path)
    locked = firmware["artifacts"]
    artifacts = {str(item["id"]): item for item in manifest["artifacts"]}  # type: ignore[index]
    required = tuple(manifest["requirements"]["required_artifacts"])  # type: ignore[index]
    report = HardwareManifestReport(
        source=str(source),
        candidate_id=str(manifest["candidate_id"]),
        artifact_ids=required,
    )

    for artifact_id in required:
        source_contact = artifacts[artifact_id]["source"]
        lock_name = source_contact["lock_artifact"]
        entry = locked.get(lock_name)
        if not isinstance(entry, dict):
            report.add(
                "lock.%s" % artifact_id,
                "artifact references an undefined firmware-lock entry",
                (artifact_id,),
                {"lock_artifact": lock_name},
            )
        elif source_contact["sha256"] != entry.get("sha256"):
            report.add(
                "lock.%s" % artifact_id,
                "artifact digest does not match firmware-lock.json",
                (artifact_id,),
                {
                    "manifest_sha256": source_contact["sha256"],
                    "locked_sha256": entry.get("sha256"),
                },
            )

    minimum = manifest["requirements"]["minimum_evidence"]  # type: ignore[index]
    for contact in REQUIRED_CONTACTS:
        observed = {
            artifact_id: _contact_value(artifacts[artifact_id]["contacts"], _CONTACT_PATHS[contact])
            for artifact_id in required
        }
        present = {artifact_id: value for artifact_id, value in observed.items() if value is not None}
        if len(present) < minimum[contact]:
            report.add(
                "coverage.%s" % contact,
                "contact has %d independent observation(s), requires %d"
                % (len(present), minimum[contact]),
                tuple(present),
                present,
            )
        if present and _distinct(present) > 1:
            report.add(
                "consistency.%s" % contact,
                "required artifacts disagree about %s" % contact,
                tuple(present),
                present,
            )
        elif len(present) >= minimum[contact]:
            report.consensus[contact] = next(iter(present.values()))

    controls = tuple(manifest["requirements"]["required_gpio_controls"])  # type: ignore[index]
    resolved_offsets: Dict[str, int] = {}
    for control in controls:
        observed_offsets = {
            artifact_id: artifacts[artifact_id]["contacts"]["gpio"]["control_offsets"][control]
            for artifact_id in required
            if control in artifacts[artifact_id]["contacts"]["gpio"]["control_offsets"]
        }
        if not observed_offsets:
            report.add(
                "gpio.mapping.%s" % control,
                "required GPIO control has no observed mapping",
            )
        elif len(set(observed_offsets.values())) > 1:
            report.add(
                "gpio.mapping.%s" % control,
                "required artifacts disagree about the GPIO offset",
                tuple(observed_offsets),
                observed_offsets,
            )
        else:
            resolved_offsets[control] = next(iter(observed_offsets.values()))

    for artifact_id in required:
        gpio = artifacts[artifact_id]["contacts"]["gpio"]
        mio_count = gpio["mio_count"]
        emio_width = gpio["emio_width"]
        if mio_count is None or emio_width is None:
            continue
        capacity = mio_count + emio_width
        out_of_range = {
            control: offset
            for control, offset in resolved_offsets.items()
            if offset >= capacity
        }
        if out_of_range:
            report.add(
                "gpio.capacity.%s" % artifact_id,
                "observed control GPIO offsets exceed the handoff's PS GPIO capacity",
                (artifact_id,),
                {"capacity": capacity, "out_of_range": out_of_range},
            )
    return report


__all__ = [
    "HARDWARE_MANIFEST_NAME",
    "HARDWARE_MANIFEST_SCHEMA",
    "HARDWARE_SCHEMA_NAME",
    "HardwareManifestIssue",
    "HardwareManifestReport",
    "P210_REQUIRED_GPIO_CONTROLS",
    "REQUIRED_CONTACTS",
    "hardware_manifest_path",
    "hardware_schema_path",
    "load_hardware_manifest",
    "validate_hardware_manifest",
]
