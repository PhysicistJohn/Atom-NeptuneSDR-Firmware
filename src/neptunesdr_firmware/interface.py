"""Load and validate the canonical Twin/Firmware interface document."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import sysconfig
from typing import Mapping, Optional

from .errors import InterfaceError


INTERFACE_NAME = "p210-firmware-interface-v1.json"
INTERFACE_SCHEMA = "neptunesdr.p210-firmware-interface/v1"


def repository_root() -> Optional[Path]:
    candidate = Path(__file__).resolve().parents[2]
    if (candidate / "pyproject.toml").is_file() and (candidate / "specs" / INTERFACE_NAME).is_file():
        return candidate
    return None


def interface_path(explicit: Optional[Path] = None) -> Path:
    if explicit is not None:
        candidate = Path(explicit)
    elif os.environ.get("NEPTUNE_FIRMWARE_INTERFACE"):
        candidate = Path(os.environ["NEPTUNE_FIRMWARE_INTERFACE"])
    else:
        root = repository_root()
        if root is not None:
            candidate = root / "specs" / INTERFACE_NAME
        else:
            data_root = Path(sysconfig.get_path("data"))
            candidate = data_root / "share" / "neptunesdr-firmware" / "specs" / INTERFACE_NAME
    if not candidate.is_file():
        raise InterfaceError("canonical interface is missing: %s" % candidate)
    return candidate.resolve()


def interface_sha256(path: Optional[Path] = None) -> str:
    return hashlib.sha256(interface_path(path).read_bytes()).hexdigest()


def load_interface(path: Optional[Path] = None) -> Mapping[str, object]:
    source = interface_path(path)
    try:
        value = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise InterfaceError("cannot read canonical interface %s: %s" % (source, exc)) from exc
    if not isinstance(value, dict):
        raise InterfaceError("canonical interface must be a JSON object")
    if value.get("schema") != INTERFACE_SCHEMA:
        raise InterfaceError("unsupported interface schema %r" % value.get("schema"))
    if value.get("profile") != "qemu-development" or value.get("flashable") is not False:
        raise InterfaceError("interface must be the non-flashable qemu-development profile")
    abi = value.get("pl_fft_abi")
    if not isinstance(abi, dict):
        raise InterfaceError("interface lacks pl_fft_abi")
    for key in ("base_address", "span_bytes", "identity", "version"):
        raw = abi.get(key)
        if not isinstance(raw, str) or not re.fullmatch(r"0x[0-9a-fA-F]+", raw):
            raise InterfaceError("pl_fft_abi.%s must be a hexadecimal string" % key)
    required_registers = (
        "ID", "VERSION", "CAPABILITIES", "CONTROL", "STATUS", "ERROR_CODE",
        "LOG2_N", "CHANNEL_COUNT", "CHANNEL_MASK", "INPUT_ADDR", "INPUT_BYTES",
        "OUTPUT_ADDR", "OUTPUT_BYTES", "SEQUENCE", "RESULT_SEQUENCE",
        "COMPLETED_LO", "COMPLETED_HI", "ERROR_COUNT_LO", "ERROR_COUNT_HI",
        "BINS_WRITTEN", "MIN_LOG2_N", "MAX_LOG2_N",
    )
    registers = abi.get("registers")
    if not isinstance(registers, dict) or tuple(registers) != required_registers:
        raise InterfaceError("pl_fft_abi.registers is not the complete ordered v1 map")
    offsets = []
    for name, raw in registers.items():
        if not isinstance(raw, str) or not re.fullmatch(r"0x[0-9a-fA-F]+", raw):
            raise InterfaceError("register %s has an invalid offset" % name)
        offsets.append(int(raw, 16))
    if offsets != list(range(0, 0x58, 4)):
        raise InterfaceError("v1 register offsets must be contiguous 32-bit words through 0x054")
    for mapping_name in ("control_bits", "status_bits", "capability_bits"):
        mapping = abi.get(mapping_name)
        if not isinstance(mapping, dict) or not mapping:
            raise InterfaceError("pl_fft_abi.%s is missing" % mapping_name)
        values = []
        for name, raw in mapping.items():
            if not isinstance(raw, str) or not re.fullmatch(r"0x[0-9a-fA-F]{8}", raw):
                raise InterfaceError("%s.%s has an invalid mask" % (mapping_name, name))
            values.append(int(raw, 16))
        if any(value <= 0 or value & (value - 1) for value in values):
            raise InterfaceError("%s values must be one-bit masks" % mapping_name)
    capabilities = sum(int(value, 16) for value in abi["capability_bits"].values())
    if capabilities != int(str(abi.get("capabilities_value")), 16):
        raise InterfaceError("capabilities_value does not combine every capability bit")
    guest_required = int(str(abi.get("guest_required_capabilities_value")), 16)
    if guest_required & ~capabilities:
        raise InterfaceError("guest-required capabilities are not a subset")
    errors = abi.get("error_codes")
    if not isinstance(errors, dict) or list(errors.values()) != list(range(11)):
        raise InterfaceError("error_codes must be the ordered v1 map 0 through 10")
    if abi.get("minimum_channels") != 1 or abi.get("maximum_channels") != 2:
        raise InterfaceError("v1 channel range must be one through two")
    if abi.get("dma_address_alignment_bytes") != 4:
        raise InterfaceError("v1 DMA addresses require four-byte alignment")
    if abi.get("input_bytes_formula") != "(1 << LOG2_N) * CHANNEL_COUNT * 4":
        raise InterfaceError("unexpected input byte formula")
    if abi.get("output_bytes_formula") != "(1 << LOG2_N) * popcount(CHANNEL_MASK) * 4":
        raise InterfaceError("unexpected output byte formula")
    stream = value.get("spectrum_stream")
    if not isinstance(stream, dict) or stream.get("protocol") != "NSFT-v1":
        raise InterfaceError("interface lacks the NSFT-v1 spectrum stream")
    return value


__all__ = [
    "INTERFACE_NAME",
    "INTERFACE_SCHEMA",
    "interface_path",
    "interface_sha256",
    "load_interface",
    "repository_root",
]
