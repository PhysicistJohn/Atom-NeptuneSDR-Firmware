"""Fail-closed orchestration for the physical P210 SD-development profile.

The checked-in board definition is deliberately allowed to be structurally
valid but unresolved.  A build can run only after every source, tool, and stage
has an immutable selection.  Commands are argv arrays executed without a shell,
and the resulting manifest covers every required board artifact.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sysconfig
import tempfile
from typing import Dict, List, Mapping, MutableMapping, Optional, Sequence, Tuple

from .errors import BoardBuildError
from .provenance import source_identity


SOURCE_LOCK_SCHEMA = "neptunesdr.board-source-lock/v1"
TOOLCHAIN_LOCK_SCHEMA = "neptunesdr.board-toolchain-lock/v1"
BOARD_PROFILE_SCHEMA = "neptunesdr.board-profile/v1"
BUILD_PLAN_SCHEMA = "neptunesdr.board-build-plan/v1"
BOARD_MANIFEST_SCHEMA = "neptunesdr.board-build-manifest/v1"

DEFINITION_FILES = (
    "source-lock.json",
    "toolchain-lock.json",
    "profile.json",
    "build-plan.json",
)

REQUIRED_SOURCE_IDS = (
    "hardware",
    "fsbl",
    "uboot",
    "linux",
    "rootfs",
    "control_daemon",
    "calibration",
    "protocol",
)

REQUIRED_OUTPUT_IDS = (
    "fsbl",
    "bitstream",
    "uboot",
    "boot_bin",
    "kernel",
    "devicetree",
    "rootfs",
    "control_daemon",
    "calibration_defaults",
    "protocol_schema",
    "timing_synthesis",
    "timing_implementation",
    "utilization_report",
    "cdc_report",
    "sd_image",
)

FORBIDDEN_EXECUTABLES = {
    "dd",
    "dfu-util",
    "flashcp",
    "fw_setenv",
    "mtd_debug",
    "nandwrite",
    "ubiformat",
}

FORBIDDEN_LAUNCHERS = {
    "bash",
    "csh",
    "dash",
    "doas",
    "env",
    "fish",
    "ksh",
    "sh",
    "sudo",
    "tcsh",
    "xargs",
    "zsh",
}

INLINE_CODE_FLAGS = {
    "node": {"-e", "--eval"},
    "perl": {"-e", "-E"},
    "php": {"-r"},
    "python": {"-c"},
    "python3": {"-c"},
    "ruby": {"-e"},
}

TOKEN_REPLACEMENTS = (
    "{repository}",
    "{board}",
    "{output}",
    "{source_date_epoch}",
)


def default_board_dir() -> Path:
    """Locate the checked-in or installed P210 board definition."""

    repository = Path(__file__).resolve().parents[2]
    candidate = repository / "board" / "p210"
    if all((candidate / name).is_file() for name in DEFINITION_FILES):
        return candidate
    installed = (
        Path(sysconfig.get_path("data"))
        / "share"
        / "neptunesdr-firmwave"
        / "board"
        / "p210"
    )
    if all((installed / name).is_file() for name in DEFINITION_FILES):
        return installed
    raise BoardBuildError("cannot locate the P210 board definition")


def _load_object(path: Path) -> Mapping[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BoardBuildError("cannot read %s: %s" % (path, exc)) from exc
    if not isinstance(value, dict):
        raise BoardBuildError("%s must contain a JSON object" % path)
    return value


def _canonical_json_sha256(path: Path) -> str:
    value = _load_object(path)
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe_relative(raw: object, label: str, *, allow_dot: bool = False) -> str:
    if not isinstance(raw, str) or not raw or "\\" in raw:
        raise BoardBuildError("%s must be a non-empty POSIX relative path" % label)
    path = PurePosixPath(raw)
    if raw == "." and allow_dot:
        return raw
    if path.is_absolute() or raw in (".", "..") or ".." in path.parts:
        raise BoardBuildError("%s escapes its declared root: %r" % (label, raw))
    return path.as_posix()


def _nonempty_string(value: object, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise BoardBuildError("%s must be a non-empty string" % label)
    return value


def _validate_status(value: Mapping[str, object], label: str) -> str:
    status = value.get("status")
    if status not in ("locked", "unresolved"):
        raise BoardBuildError("%s status must be locked or unresolved" % label)
    if status == "unresolved":
        _nonempty_string(value.get("reason"), label + ".reason")
    return str(status)


def validate_source_lock(value: Mapping[str, object]) -> Mapping[str, object]:
    if value.get("schema") != SOURCE_LOCK_SCHEMA:
        raise BoardBuildError("unsupported board source-lock schema")
    platform_id = _nonempty_string(value.get("platform_id"), "source-lock.platform_id")
    _nonempty_string(value.get("profile"), "source-lock.profile")
    sources = value.get("sources")
    if not isinstance(sources, dict) or tuple(sources) != REQUIRED_SOURCE_IDS:
        raise BoardBuildError("source-lock must contain the complete ordered source set")
    for name, raw in sources.items():
        if not isinstance(raw, dict):
            raise BoardBuildError("source %s must be an object" % name)
        if raw.get("platform_id") != platform_id:
            raise BoardBuildError("source %s is not matched to %s" % (name, platform_id))
        status = _validate_status(raw, "source-lock.sources.%s" % name)
        if status == "unresolved":
            continue
        kind = raw.get("kind")
        if kind == "git":
            url = raw.get("url")
            commit = raw.get("commit")
            if not isinstance(url, str) or not url.startswith("https://"):
                raise BoardBuildError("locked Git source %s must use HTTPS" % name)
            if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40}", commit):
                raise BoardBuildError("locked Git source %s needs a 40-hex commit" % name)
            _safe_relative(
                raw.get("path"),
                "source-lock.sources.%s.path" % name,
                allow_dot=True,
            )
        elif kind == "file":
            _safe_relative(raw.get("path"), "source-lock.sources.%s.path" % name)
            digest = raw.get("sha256")
            size = raw.get("bytes")
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise BoardBuildError("locked file source %s needs a SHA-256" % name)
            if isinstance(size, bool) or not isinstance(size, int) or size < 0:
                raise BoardBuildError("locked file source %s has an invalid byte count" % name)
        else:
            raise BoardBuildError("locked source %s has unsupported kind %r" % (name, kind))
    return value


def validate_toolchain_lock(value: Mapping[str, object], platform_id: str) -> Mapping[str, object]:
    if value.get("schema") != TOOLCHAIN_LOCK_SCHEMA:
        raise BoardBuildError("unsupported board toolchain-lock schema")
    if value.get("platform_id") != platform_id:
        raise BoardBuildError("toolchain lock is not matched to %s" % platform_id)
    tools = value.get("tools")
    if not isinstance(tools, dict) or not tools:
        raise BoardBuildError("toolchain lock has no tools")
    for name, raw in tools.items():
        if not isinstance(name, str) or not name or not isinstance(raw, dict):
            raise BoardBuildError("toolchain entries must be named objects")
        status = _validate_status(raw, "toolchain-lock.tools.%s" % name)
        if status == "unresolved":
            continue
        command = raw.get("command")
        arguments = raw.get("version_args")
        pattern = raw.get("version_regex")
        if (
            not isinstance(command, str)
            or not command
            or any(character.isspace() for character in command)
        ):
            raise BoardBuildError("tool %s has an invalid command" % name)
        if not isinstance(arguments, list) or not all(isinstance(item, str) for item in arguments):
            raise BoardBuildError("tool %s version_args must be strings" % name)
        if not isinstance(pattern, str) or not pattern:
            raise BoardBuildError("tool %s needs a version_regex" % name)
        try:
            re.compile(pattern)
        except re.error as exc:
            raise BoardBuildError("tool %s has an invalid version regex" % name) from exc
    return value


def validate_profile(value: Mapping[str, object], platform_id: str) -> Mapping[str, object]:
    if value.get("schema") != BOARD_PROFILE_SCHEMA:
        raise BoardBuildError("unsupported board profile schema")
    if value.get("platform_id") != platform_id:
        raise BoardBuildError("board profile is not matched to %s" % platform_id)
    if value.get("execution_target") != "physical-board":
        raise BoardBuildError("board profile must target the physical board")
    if value.get("flashable") is not False or value.get("physical_validation") != "required":
        raise BoardBuildError("board profile must remain non-flashable and unvalidated")
    install = value.get("installation")
    if not isinstance(install, dict) or install != {
        "medium": "removable-sd",
        "host_writer_included": False,
        "qspi_write_allowed": False,
    }:
        raise BoardBuildError("board installation policy must be SD-only with no writer")
    safety = value.get("safety")
    required_safety = {
        "tx_enabled_at_boot": False,
        "tx_requires_explicit_runtime_enable": True,
        "qspi_write_allowed": False,
        "raw_bypass_required": True,
    }
    if not isinstance(safety, dict) or safety != required_safety:
        raise BoardBuildError("board safety policy is incomplete")
    outputs = value.get("outputs")
    if not isinstance(outputs, list):
        raise BoardBuildError("board profile outputs must be an array")
    observed_ids: List[str] = []
    observed_paths: List[str] = []
    for index, raw in enumerate(outputs):
        if not isinstance(raw, dict):
            raise BoardBuildError("profile output %d must be an object" % index)
        name = _nonempty_string(raw.get("id"), "profile.outputs[%d].id" % index)
        observed_ids.append(name)
        observed_paths.append(_safe_relative(raw.get("path"), "profile output %s path" % name))
        _nonempty_string(raw.get("role"), "profile output %s role" % name)
    if tuple(observed_ids) != REQUIRED_OUTPUT_IDS:
        raise BoardBuildError("board profile must contain the complete ordered output set")
    if len(observed_paths) != len(set(observed_paths)):
        raise BoardBuildError("board profile output paths must be unique")
    return value


def _validate_command(command: object, stage: str) -> Tuple[str, ...]:
    if not isinstance(command, list) or not command or not all(
        isinstance(item, str) and item for item in command
    ):
        raise BoardBuildError("locked stage %s needs a non-empty argv array" % stage)
    executable = PurePosixPath(command[0]).name
    if executable in FORBIDDEN_EXECUTABLES:
        raise BoardBuildError("stage %s invokes forbidden writer %s" % (stage, executable))
    if executable in FORBIDDEN_LAUNCHERS:
        raise BoardBuildError("stage %s invokes forbidden command launcher %s" % (stage, executable))
    inline_flags = INLINE_CODE_FLAGS.get(executable)
    if inline_flags is None and executable.startswith("python"):
        inline_flags = {"-c"}
    if inline_flags is not None and any(item in inline_flags for item in command[1:]):
        raise BoardBuildError("stage %s embeds unauditable inline interpreter code" % stage)
    for item in command:
        if any(ord(character) < 32 for character in item):
            raise BoardBuildError("stage %s contains control characters" % stage)
        if item == "/dev" or "/dev/" in item:
            raise BoardBuildError("stage %s may not target a device node" % stage)
        unknown = re.findall(r"\{[^{}]+\}", item)
        if any(token not in TOKEN_REPLACEMENTS for token in unknown):
            raise BoardBuildError("stage %s contains an unknown command token" % stage)
    return tuple(command)


def _topological_order(stages: Mapping[str, Mapping[str, object]]) -> Tuple[str, ...]:
    pending = list(stages)
    complete: List[str] = []
    while pending:
        ready = [
            name
            for name in pending
            if all(dependency in complete for dependency in stages[name]["depends"])
        ]
        if not ready:
            raise BoardBuildError("build plan contains a dependency cycle")
        for name in ready:
            pending.remove(name)
            complete.append(name)
    return tuple(complete)


def validate_build_plan(
    value: Mapping[str, object],
    platform_id: str,
    profile_name: str,
) -> Tuple[Mapping[str, object], Tuple[str, ...]]:
    if value.get("schema") != BUILD_PLAN_SCHEMA:
        raise BoardBuildError("unsupported board build-plan schema")
    if value.get("platform_id") != platform_id or value.get("profile") != profile_name:
        raise BoardBuildError("build plan is not matched to the board profile")
    epoch = value.get("source_date_epoch")
    if isinstance(epoch, bool) or not isinstance(epoch, int) or epoch < 0:
        raise BoardBuildError("build plan needs a non-negative source_date_epoch")
    raw_stages = value.get("stages")
    if not isinstance(raw_stages, list) or not raw_stages:
        raise BoardBuildError("build plan has no stages")
    stages: Dict[str, Mapping[str, object]] = {}
    assigned_outputs: List[str] = []
    for index, raw in enumerate(raw_stages):
        if not isinstance(raw, dict):
            raise BoardBuildError("build stage %d must be an object" % index)
        name = _nonempty_string(raw.get("id"), "build-plan.stages[%d].id" % index)
        if name in stages:
            raise BoardBuildError("duplicate build stage %s" % name)
        depends = raw.get("depends")
        outputs = raw.get("outputs")
        if not isinstance(depends, list) or not all(isinstance(item, str) for item in depends):
            raise BoardBuildError("stage %s dependencies must be strings" % name)
        if not isinstance(outputs, list) or not outputs or not all(
            isinstance(item, str) for item in outputs
        ):
            raise BoardBuildError("stage %s outputs must be non-empty strings" % name)
        if len(outputs) != len(set(outputs)):
            raise BoardBuildError("stage %s repeats an output" % name)
        status = _validate_status(raw, "build-plan.stages.%s" % name)
        if status == "locked":
            _validate_command(raw.get("command"), name)
        elif raw.get("command") not in (None, []):
            raise BoardBuildError("unresolved stage %s may not carry a command" % name)
        stages[name] = raw
        assigned_outputs.extend(outputs)
    for name, raw in stages.items():
        unknown = sorted(set(raw["depends"]) - set(stages))
        if unknown or name in raw["depends"]:
            raise BoardBuildError("stage %s has invalid dependencies" % name)
    if tuple(assigned_outputs) != REQUIRED_OUTPUT_IDS:
        raise BoardBuildError("build stages must assign every profile output exactly once")
    return value, _topological_order(stages)


def load_board_definition(board_dir: Path) -> Mapping[str, object]:
    board = Path(board_dir).resolve()
    missing = [name for name in DEFINITION_FILES if not (board / name).is_file()]
    if missing:
        raise BoardBuildError("board definition is missing: %s" % ", ".join(missing))
    source_lock = validate_source_lock(_load_object(board / "source-lock.json"))
    platform_id = str(source_lock["platform_id"])
    toolchain = validate_toolchain_lock(
        _load_object(board / "toolchain-lock.json"), platform_id
    )
    profile = validate_profile(_load_object(board / "profile.json"), platform_id)
    plan, order = validate_build_plan(
        _load_object(board / "build-plan.json"),
        platform_id,
        str(profile.get("profile")),
    )
    return {
        "board_dir": board,
        "source_lock": source_lock,
        "toolchain_lock": toolchain,
        "profile": profile,
        "build_plan": plan,
        "stage_order": order,
    }


def _resolve_tool(command: str) -> Optional[Path]:
    candidate = Path(command)
    if candidate.is_absolute() or "/" in command:
        return candidate.resolve() if candidate.is_file() and os.access(candidate, os.X_OK) else None
    located = shutil.which(command)
    return Path(located).resolve() if located else None


def _inspect_tools(tools: Mapping[str, object]) -> Tuple[Mapping[str, object], List[str]]:
    observations: Dict[str, object] = {}
    blockers: List[str] = []
    for name, raw_object in tools.items():
        raw = raw_object
        if raw["status"] == "unresolved":
            blockers.append("tool %s is unresolved: %s" % (name, raw["reason"]))
            observations[name] = {"status": "unresolved"}
            continue
        located = _resolve_tool(str(raw["command"]))
        if located is None:
            blockers.append("tool %s is not installed: %s" % (name, raw["command"]))
            observations[name] = {"status": "missing"}
            continue
        try:
            result = subprocess.run(
                (str(located), *raw["version_args"]),
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=15,
                env={**os.environ, "LC_ALL": "C", "TZ": "UTC"},
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            blockers.append("tool %s version probe failed: %s" % (name, exc))
            observations[name] = {"status": "probe-failed"}
            continue
        rendered = result.stdout.decode("utf-8", errors="replace").strip()
        digest = hashlib.sha256(result.stdout).hexdigest()
        if result.returncode != 0 or re.search(str(raw["version_regex"]), rendered) is None:
            blockers.append("tool %s does not match its locked version" % name)
            status = "version-mismatch"
        else:
            status = "matched"
        observations[name] = {
            "status": status,
            "version_output_sha256": digest,
            "version_first_line": rendered.splitlines()[0] if rendered else "",
        }
    return observations, blockers


def _inspect_sources(
    sources: Mapping[str, object], repository: Path
) -> Tuple[Mapping[str, object], List[str]]:
    observations: Dict[str, object] = {}
    blockers: List[str] = []
    for name, raw_object in sources.items():
        raw = raw_object
        if raw["status"] == "unresolved":
            blockers.append("source %s is unresolved: %s" % (name, raw["reason"]))
            observations[name] = {"status": "unresolved"}
            continue
        path = (repository / str(raw["path"])).resolve()
        try:
            path.relative_to(repository)
        except ValueError:
            blockers.append("source %s path escapes the repository" % name)
            observations[name] = {"status": "invalid-path"}
            continue
        if raw["kind"] == "file":
            if path.is_symlink() or not path.is_file():
                blockers.append("source %s file is missing" % name)
                observations[name] = {"status": "missing"}
                continue
            actual_size = path.stat().st_size
            actual_sha = _sha256_file(path)
            matched = actual_size == raw["bytes"] and actual_sha == raw["sha256"]
            observations[name] = {
                "status": "matched" if matched else "digest-mismatch",
                "bytes": actual_size,
                "sha256": actual_sha,
            }
            if not matched:
                blockers.append("source %s file does not match its lock" % name)
        else:
            if not (path / ".git").exists() and not path.is_dir():
                blockers.append("source %s Git checkout is missing" % name)
                observations[name] = {"status": "missing"}
                continue
            try:
                commit = subprocess.check_output(
                    ("git", "-C", str(path), "rev-parse", "HEAD"),
                    stderr=subprocess.STDOUT,
                ).decode("ascii").strip()
                dirty = subprocess.check_output(
                    (
                        "git",
                        "-C",
                        str(path),
                        "status",
                        "--porcelain=v1",
                        "--untracked-files=all",
                    ),
                    stderr=subprocess.STDOUT,
                )
            except (OSError, UnicodeError, subprocess.CalledProcessError):
                commit = ""
                dirty = b"invalid-checkout"
            matched = commit == raw["commit"] and not dirty
            observations[name] = {
                "status": "matched" if matched else "source-state-mismatch",
                "commit": commit,
                "clean": not dirty,
            }
            if not matched:
                blockers.append("source %s checkout is not clean at its locked commit" % name)
    return observations, blockers


def doctor_board(
    board_dir: Path,
    repository: Path,
    *,
    check_environment: bool = True,
) -> Mapping[str, object]:
    """Validate a board definition and report why a real build is blocked."""

    definition = load_board_definition(board_dir)
    repository = Path(repository).resolve()
    blockers: List[str] = []
    source_observations: Mapping[str, object] = {}
    tool_observations: Mapping[str, object] = {}
    if check_environment:
        source_observations, source_blockers = _inspect_sources(
            definition["source_lock"]["sources"], repository
        )
        tool_observations, tool_blockers = _inspect_tools(
            definition["toolchain_lock"]["tools"]
        )
        blockers.extend(source_blockers)
        blockers.extend(tool_blockers)
    else:
        for name, raw in definition["source_lock"]["sources"].items():
            if raw["status"] == "unresolved":
                blockers.append("source %s is unresolved: %s" % (name, raw["reason"]))
        for name, raw in definition["toolchain_lock"]["tools"].items():
            if raw["status"] == "unresolved":
                blockers.append("tool %s is unresolved: %s" % (name, raw["reason"]))
    for stage in definition["build_plan"]["stages"]:
        if stage["status"] == "unresolved":
            blockers.append("stage %s is unresolved: %s" % (stage["id"], stage["reason"]))
    return {
        "schema": "neptunesdr.board-doctor-report/v1",
        "valid": True,
        "ready": not blockers,
        "profile": definition["profile"]["profile"],
        "platform_id": definition["profile"]["platform_id"],
        "flashable": False,
        "qspi_write_allowed": False,
        "tx_enabled_at_boot": False,
        "stage_order": list(definition["stage_order"]),
        "blockers": sorted(blockers),
        "sources": source_observations,
        "tools": tool_observations,
    }


def _expanded_command(
    command: Sequence[str],
    *,
    repository: Path,
    board: Path,
    output: Path,
    epoch: int,
) -> Tuple[str, ...]:
    replacements = {
        "{repository}": str(repository),
        "{board}": str(board),
        "{output}": str(output),
        "{source_date_epoch}": str(epoch),
    }
    result: List[str] = []
    for raw in command:
        rendered = raw
        for token, value in replacements.items():
            rendered = rendered.replace(token, value)
        result.append(rendered)
    _validate_command(result, "expanded")
    return tuple(result)


def _artifact_records(profile: Mapping[str, object], output: Path) -> Mapping[str, object]:
    records: Dict[str, object] = {}
    for raw in profile["outputs"]:
        name = str(raw["id"])
        relative = str(raw["path"])
        path = output / relative
        if path.is_symlink() or not path.is_file():
            raise BoardBuildError("required output %s is missing or not a regular file: %s" % (name, path))
        resolved = path.resolve()
        try:
            resolved.relative_to(output)
        except ValueError as exc:
            raise BoardBuildError("required output %s escapes the output root" % name) from exc
        records[name] = {
            "path": relative,
            "bytes": resolved.stat().st_size,
            "sha256": _sha256_file(resolved),
            "role": raw["role"],
        }
    if tuple(records) != REQUIRED_OUTPUT_IDS:
        raise BoardBuildError("output manifest coverage is incomplete")
    return records


def _write_manifest(path: Path, manifest: Mapping[str, object]) -> Path:
    payload = json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8") + b"\n"
    descriptor, name = tempfile.mkstemp(prefix=path.name + ".", suffix=".part", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()
    return path


def build_board(
    board_dir: Path,
    repository: Path,
    output_dir: Path,
) -> Tuple[Mapping[str, object], Path]:
    """Execute a fully locked plan and emit a complete deterministic manifest."""

    definition = load_board_definition(board_dir)
    repository = Path(repository).resolve()
    report = doctor_board(board_dir, repository, check_environment=True)
    if not report["ready"]:
        raise BoardBuildError("board build is blocked: " + "; ".join(report["blockers"]))
    output = Path(output_dir).resolve()
    device_root = Path("/dev").resolve()
    if (
        output in (Path("/").resolve(), Path.home().resolve(), repository, device_root)
        or device_root in output.parents
    ):
        raise BoardBuildError("refusing a broad board output directory: %s" % output)
    if output.exists() and not output.is_dir():
        raise BoardBuildError("board output is not a directory: %s" % output)
    output.mkdir(parents=True, exist_ok=True)
    board = Path(board_dir).resolve()
    epoch = int(definition["build_plan"]["source_date_epoch"])
    stages = {str(stage["id"]): stage for stage in definition["build_plan"]["stages"]}
    environment = {
        **os.environ,
        "LC_ALL": "C",
        "TZ": "UTC",
        "SOURCE_DATE_EPOCH": str(epoch),
        "NEPTUNE_BOARD_DIR": str(board),
        "NEPTUNE_BOARD_OUTPUT": str(output),
    }
    for name in definition["stage_order"]:
        stage = stages[name]
        command = _expanded_command(
            stage["command"],
            repository=repository,
            board=board,
            output=output,
            epoch=epoch,
        )
        try:
            result = subprocess.run(command, cwd=repository, env=environment, check=False)
        except OSError as exc:
            raise BoardBuildError("cannot execute stage %s: %s" % (name, exc)) from exc
        if result.returncode != 0:
            raise BoardBuildError("stage %s failed with status %d" % (name, result.returncode))
    records = _artifact_records(definition["profile"], output)
    definition_hashes = {
        name: _canonical_json_sha256(board / name) for name in DEFINITION_FILES
    }
    manifest: MutableMapping[str, object] = {
        "schema": BOARD_MANIFEST_SCHEMA,
        "profile": definition["profile"]["profile"],
        "platform_id": definition["profile"]["platform_id"],
        "execution_target": "physical-board",
        "physical_validation": False,
        "flashable": False,
        "installation": dict(definition["profile"]["installation"]),
        "safety": dict(definition["profile"]["safety"]),
        "source_date_epoch": epoch,
        "definition_sha256": definition_hashes,
        "firmwave_source": source_identity(repository),
        "tool_observations": report["tools"],
        "generated_artifacts": records,
        "artifact_hashes_complete": True,
        "artifact_scope": "every required board output except this self-describing manifest",
        "limitations": [
            "Artifact construction does not prove physical RF, timing, USB, recovery, or TX-inhibit behavior.",
            "No host block-device writer or QSPI update path is included.",
        ],
    }
    manifest_path = _write_manifest(output / "board-build-manifest.json", manifest)
    return manifest, manifest_path


__all__ = [
    "BOARD_MANIFEST_SCHEMA",
    "BOARD_PROFILE_SCHEMA",
    "BUILD_PLAN_SCHEMA",
    "DEFINITION_FILES",
    "REQUIRED_OUTPUT_IDS",
    "REQUIRED_SOURCE_IDS",
    "SOURCE_LOCK_SCHEMA",
    "TOOLCHAIN_LOCK_SCHEMA",
    "build_board",
    "default_board_dir",
    "doctor_board",
    "load_board_definition",
    "validate_build_plan",
    "validate_profile",
    "validate_source_lock",
    "validate_toolchain_lock",
]
