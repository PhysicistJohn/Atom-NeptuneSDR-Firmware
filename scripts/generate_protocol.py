#!/usr/bin/env python3
"""Generate and verify Neptune Edge v1 wire-contract bindings and vectors."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple


ROOT = Path(__file__).resolve().parents[1]
DATA_SPEC_PATH = ROOT / "specs" / "neptune-edge-data-v1.json"
CONTROL_SPEC_PATH = ROOT / "specs" / "neptune-edge-control-v1.json"
PYTHON_OUTPUT = ROOT / "protocol" / "generated" / "neptune_edge_v1.py"
C_OUTPUT = ROOT / "protocol" / "generated" / "neptune_edge_v1.h"
GOLDEN_OUTPUT = ROOT / "protocol" / "golden" / "neptune_edge_v1_vectors.json"

TYPE_INFO = {
    "u8": (1, "B", "uint8_t"),
    "i8": (1, "b", "int8_t"),
    "u16": (2, "H", "uint16_t"),
    "i16": (2, "h", "int16_t"),
    "u32": (4, "I", "uint32_t"),
    "i32": (4, "i", "int32_t"),
    "u64": (8, "Q", "uint64_t"),
    "i64": (8, "q", "int64_t"),
    "bytes4": (4, "4s", "uint8_t"),
    "bytes32": (32, "32s", "uint8_t"),
    "bytes256": (256, "256s", "uint8_t"),
}


def _json(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError("%s must contain a JSON object" % path)
    return value


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _constant_name(value: str) -> str:
    return "".join(character if character.isalnum() else "_" for character in value.upper())


def _python_class_name(value: str) -> str:
    return "".join(part.title() for part in value.split("_"))


def _validate_fields(label: str, fields: Sequence[Mapping[str, Any]], start: int, end: int) -> None:
    cursor = start
    names = set()
    for field in fields:
        name = field["name"]
        type_name = field["type"]
        if name in names:
            raise ValueError("%s duplicates field %s" % (label, name))
        names.add(name)
        if type_name not in TYPE_INFO:
            raise ValueError("%s.%s has unknown type %s" % (label, name, type_name))
        if field["offset"] != cursor:
            raise ValueError(
                "%s.%s offset is %d, expected contiguous offset %d"
                % (label, name, field["offset"], cursor)
            )
        cursor += TYPE_INFO[type_name][0]
    if cursor != end:
        raise ValueError("%s fields end at %d, expected %d" % (label, cursor, end))


def _validate_enum(label: str, values: Mapping[str, Any], maximum: int) -> None:
    seen = set()
    for name, value in values.items():
        if not isinstance(value, int) or value < 0 or value > maximum:
            raise ValueError("%s.%s is outside 0..%d" % (label, name, maximum))
        if value in seen:
            raise ValueError("%s duplicates numeric value %d" % (label, value))
        seen.add(value)


def validate_specs(data: Mapping[str, Any], control: Mapping[str, Any]) -> None:
    if data.get("schema") != "neptunesdr.edge.data-schema/v1":
        raise ValueError("unexpected data schema")
    if control.get("schema") != "neptunesdr.edge.control-schema/v1":
        raise ValueError("unexpected control schema")
    if data["wire"]["byte_order"] != "little-endian":
        raise ValueError("v1 data byte order must be little-endian")
    if control["transport"]["byte_order"] != "little-endian":
        raise ValueError("v1 control byte order must be little-endian")

    clock = data["clock_model"]
    internal = clock["internal_sample_rate_hz"]
    output = clock["continuous_egress_sample_rate_hz"]
    interpolation = clock["interpolation"]
    decimation = clock["decimation"]
    if (internal, output, interpolation, decimation) != (61_440_000, 55_000_000, 1375, 1536):
        raise ValueError("v1 clock model is fixed at 61.44 MSPS -> 55 MSPS by 1375/1536")
    if internal * interpolation != output * decimation:
        raise ValueError("resampler identity is not exact")
    if clock["timestamp_timebase_hz"] != internal or clock["timestamp_bits"] != 64:
        raise ValueError("sample timestamps must be 64-bit 61.44 MHz ingress ticks")
    rate_domains = data["rate_domains"]
    if rate_domains["RAW_IQ"]["sample_rate_hz"] != internal:
        raise ValueError("RAW_IQ must remain in the native ingress rate domain")
    if rate_domains["NORMALIZED_IQ"]["sample_rate_hz"] != output:
        raise ValueError("NORMALIZED_IQ must use the canonical egress rate domain")
    if rate_domains["CALIBRATED_IQ"]["sample_rate_hz"] != [internal, output]:
        raise ValueError("CALIBRATED_IQ rate domains changed incompatibly")

    data_header = data["base_header"]
    control_header = control["base_header"]
    if (data_header["magic_ascii"], data_header["version"], data_header["size_bytes"]) != (
        "NEDP",
        1,
        64,
    ):
        raise ValueError("v1 data base header identity or size changed")
    if (control_header["magic_ascii"], control_header["version"], control_header["size_bytes"]) != (
        "NECP",
        1,
        40,
    ):
        raise ValueError("v1 control base header identity or size changed")
    _validate_fields("data.base_header", data_header["fields"], 0, 64)
    _validate_fields("data.extension_header", data["extension_header"]["fields"], 0, 8)
    _validate_fields("control.base_header", control_header["fields"], 0, 40)
    _validate_fields("control.item_header", control["item_header"]["fields"], 0, 8)

    for family, enum_values in data["enums"].items():
        _validate_enum("data.enums.%s" % family, enum_values, (1 << 64) - 1)
    for family, enum_values in control["enums"].items():
        _validate_enum("control.enums.%s" % family, enum_values, 65535)

    for category, definitions in (("data.extensions", data["extensions"]), ("control.items", control["items"])):
        codes = set()
        for name, definition in definitions.items():
            code = definition["code"]
            size = definition["size_bytes"]
            if not isinstance(code, int) or not 0 < code <= 65535 or code in codes:
                raise ValueError("%s.%s has invalid or duplicate code" % (category, name))
            codes.add(code)
            if size < 8 or size % 4:
                raise ValueError("%s.%s size is not four-byte aligned" % (category, name))
            _validate_fields("%s.%s" % (category, name), definition["fields"], 8, size)

    formats = data["sample_formats"]
    for required, code, bits in (("S16", 1, 32), ("S12P", 2, 24), ("S8", 3, 16), ("S8BF", 4, 16)):
        if formats[required]["code"] != code or formats[required]["bits_per_complex_sample"] != bits:
            raise ValueError("sample format %s changed incompatibly" % required)
        if data["enums"]["sample_format"][required] != code:
            raise ValueError("sample format enum and layout disagree for %s" % required)

    rf_constraints = control["items"]["RF_CONFIG"]["constraints"]
    if rf_constraints["internal_sample_rate_hz"] != [internal]:
        raise ValueError("RF_CONFIG internal rate does not refine the clock model")
    if rf_constraints["egress_sample_rate_hz"] != [output]:
        raise ValueError("RF_CONFIG egress rate does not refine the clock model")

    commands = control["enums"]["command"]
    contracts = control.get("command_contracts", {})
    if set(contracts) != set(commands):
        raise ValueError(
            "command contracts do not cover the command enum: missing=%s extra=%s"
            % (sorted(set(commands) - set(contracts)), sorted(set(contracts) - set(commands)))
        )
    item_codes = {name: definition["code"] for name, definition in control["items"].items()}
    for command_name, contract in contracts.items():
        if set(contract) != {"request", "response"}:
            raise ValueError("command contract %s must contain request and response" % command_name)
        for direction in ("request", "response"):
            names = contract[direction]
            if len(names) != len(set(names)):
                raise ValueError("command contract %s.%s duplicates an item" % (command_name, direction))
            unknown = set(names) - set(item_codes)
            if unknown:
                raise ValueError(
                    "command contract %s.%s has unknown items %s"
                    % (command_name, direction, sorted(unknown))
                )
            if [item_codes[name] for name in names] != sorted(item_codes[name] for name in names):
                raise ValueError(
                    "command contract %s.%s is not in canonical item-code order"
                    % (command_name, direction)
                )


def _struct_format(fields: Sequence[Mapping[str, Any]]) -> str:
    return "<" + "".join(TYPE_INFO[field["type"]][1] for field in fields)


def _python_definition_table(definitions: Mapping[str, Any]) -> str:
    lines = ["{"]
    for name, definition in sorted(definitions.items(), key=lambda item: item[1]["code"]):
        fields = definition["fields"]
        names = tuple(field["name"] for field in fields)
        lines.append(
            "    %r: (%r, %d, struct.Struct(%r), %r),"
            % (name, definition["code"], definition["size_bytes"], _struct_format(fields), names)
        )
    lines.append("}")
    return "\n".join(lines)


def _python_command_contracts(control: Mapping[str, Any]) -> str:
    commands = control["enums"]["command"]
    contracts = control["command_contracts"]
    values = {
        commands[name]: {
            "request": tuple(contract["request"]),
            "response": tuple(contract["response"]),
        }
        for name, contract in contracts.items()
    }
    return repr(dict(sorted(values.items())))


def _render_python(data: Mapping[str, Any], control: Mapping[str, Any]) -> str:
    data_sha = _sha256(DATA_SPEC_PATH.read_bytes())
    control_sha = _sha256(CONTROL_SPEC_PATH.read_bytes())
    enum_blocks: List[str] = []
    flag_families = {"data_flag", "changed_field", "control_flag", "item_flag"}
    all_enums: List[Tuple[str, Mapping[str, int]]] = []
    for family, values in data["enums"].items():
        all_enums.append((family, values))
    for family, values in control["enums"].items():
        all_enums.append((family, values))
    for family, values in all_enums:
        base = "IntFlag" if family in flag_families else "IntEnum"
        enum_blocks.append("class %s(%s):" % (_python_class_name(family), base))
        for name, value in values.items():
            enum_blocks.append("    %s = %d" % (name, value))
        enum_blocks.append("")

    data_fields = tuple(field["name"] for field in data["base_header"]["fields"])
    control_fields = tuple(field["name"] for field in control["base_header"]["fields"])
    data_extensions = _python_definition_table(data["extensions"])
    control_items = _python_definition_table(control["items"])
    control_contracts = _python_command_contracts(control)
    return '''# Generated by scripts/generate_protocol.py; DO NOT EDIT.
"""Neptune Edge v1 data/control wire binding (Python 3.9+)."""

from enum import IntEnum, IntFlag
import struct

DATA_SPEC_SHA256 = %r
CONTROL_SPEC_SHA256 = %r
DATA_MAGIC = b"NEDP"
CONTROL_MAGIC = b"NECP"
PROTOCOL_VERSION = 1
DATA_HEADER_BYTES = 64
CONTROL_HEADER_BYTES = 40
TYPED_HEADER_BYTES = 8
INTERNAL_SAMPLE_RATE_HZ = 61_440_000
EGRESS_SAMPLE_RATE_HZ = 55_000_000
RESAMPLER_INTERPOLATION = 1375
RESAMPLER_DECIMATION = 1536
UINT64_MAX = (1 << 64) - 1

%s
DATA_HEADER_STRUCT = struct.Struct(%r)
CONTROL_HEADER_STRUCT = struct.Struct(%r)
TYPED_HEADER_STRUCT = struct.Struct("<HHI")
DATA_HEADER_FIELDS = %r
CONTROL_HEADER_FIELDS = %r
DATA_EXTENSION_DEFINITIONS = %s
CONTROL_ITEM_DEFINITIONS = %s
DATA_EXTENSION_BY_CODE = {value[0]: (name,) + value[1:] for name, value in DATA_EXTENSION_DEFINITIONS.items()}
CONTROL_ITEM_BY_CODE = {value[0]: (name,) + value[1:] for name, value in CONTROL_ITEM_DEFINITIONS.items()}
CONTROL_COMMAND_CONTRACTS = %s


class ProtocolError(ValueError):
    """A malformed or semantically inconsistent Neptune Edge message."""


def crc32c(data):
    """Return standard reflected CRC-32C (Castagnoli)."""
    crc = 0xFFFFFFFF
    for octet in bytes(data):
        crc ^= octet
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def round_shift_nearest_even(value, shift):
    """Round a signed integer right shift without floating point or bias."""
    if shift < 0:
        raise ProtocolError("shift must be nonnegative")
    if shift == 0:
        return int(value)
    magnitude = abs(int(value))
    quotient, remainder = divmod(magnitude, 1 << shift)
    halfway = 1 << (shift - 1)
    if remainder > halfway or (remainder == halfway and quotient & 1):
        quotient += 1
    return -quotient if value < 0 else quotient


def quantize_s8(value, shift):
    """Round-to-nearest-even and saturate to signed two's-complement int8."""
    rounded = round_shift_nearest_even(value, shift)
    return max(-128, min(127, rounded))


def pack_iq_samples(sample_format, samples):
    """Pack one channel's iterable of (I, Q) pairs in a canonical IQ format."""
    sample_format = int(sample_format)
    output = bytearray()
    for i_value, q_value in samples:
        i_value = int(i_value)
        q_value = int(q_value)
        if sample_format in (int(SampleFormat.S16), int(SampleFormat.S12P)):
            if not (-2048 <= i_value <= 2047 and -2048 <= q_value <= 2047):
                raise ProtocolError("S16/S12P values must be native signed 12-bit ADC codes")
        elif sample_format in (int(SampleFormat.S8), int(SampleFormat.S8BF)):
            if not (-128 <= i_value <= 127 and -128 <= q_value <= 127):
                raise ProtocolError("S8/S8BF values must fit signed int8")
        else:
            raise ProtocolError("sample format does not carry integer IQ")
        if sample_format == int(SampleFormat.S16):
            output.extend(struct.pack("<hh", i_value, q_value))
        elif sample_format == int(SampleFormat.S12P):
            word = (i_value & 0xFFF) | ((q_value & 0xFFF) << 12)
            output.extend((word & 0xFF, (word >> 8) & 0xFF, (word >> 16) & 0xFF))
        else:
            output.extend(struct.pack("<bb", i_value, q_value))
    return bytes(output)


def unpack_iq_samples(sample_format, payload):
    """Unpack a canonical IQ payload into a tuple of signed (I, Q) pairs."""
    sample_format = int(sample_format)
    payload = bytes(payload)
    stride = {
        int(SampleFormat.S16): 4,
        int(SampleFormat.S12P): 3,
        int(SampleFormat.S8): 2,
        int(SampleFormat.S8BF): 2,
    }.get(sample_format)
    if stride is None or len(payload) %% stride:
        raise ProtocolError("IQ payload length is not valid for the sample format")
    values = []
    for offset in range(0, len(payload), stride):
        if sample_format == int(SampleFormat.S16):
            pair = struct.unpack_from("<hh", payload, offset)
        elif sample_format == int(SampleFormat.S12P):
            word = payload[offset] | (payload[offset + 1] << 8) | (payload[offset + 2] << 16)
            i_value = word & 0xFFF
            q_value = (word >> 12) & 0xFFF
            pair = (i_value - 4096 if i_value & 0x800 else i_value,
                    q_value - 4096 if q_value & 0x800 else q_value)
        else:
            pair = struct.unpack_from("<bb", payload, offset)
        values.append(pair)
    return tuple(values)


def advance_resampler(input_timestamp, phase_numerator, sample_count):
    """Advance the exact 1375/1536 resampler state without floating point."""
    if not 0 <= phase_numerator < RESAMPLER_INTERPOLATION:
        raise ProtocolError("resampler phase is outside 0..1374")
    if sample_count < 0:
        raise ProtocolError("sample_count must be nonnegative")
    total = phase_numerator + sample_count * RESAMPLER_DECIMATION
    return input_timestamp + total // RESAMPLER_INTERPOLATION, total %% RESAMPLER_INTERPOLATION


def source_tick(input_timestamp, phase_numerator, output_index):
    """Map a packet-local 55 MHz output index to a 61.44 MHz ingress tick."""
    if not 0 <= phase_numerator < RESAMPLER_INTERPOLATION:
        raise ProtocolError("resampler phase is outside 0..1374")
    if output_index < 0:
        raise ProtocolError("output_index must be nonnegative")
    return input_timestamp + (phase_numerator + output_index * RESAMPLER_DECIMATION) // RESAMPLER_INTERPOLATION


def _pack_typed(definitions, name, flags, values):
    try:
        code, size, body_struct, field_names = definitions[name]
    except KeyError as error:
        raise ProtocolError("unknown typed structure %%s" %% name) from error
    missing = set(field_names) - set(values)
    extra = set(values) - set(field_names)
    if missing or extra:
        raise ProtocolError("%%s fields missing=%%s extra=%%s" %% (name, sorted(missing), sorted(extra)))
    body = body_struct.pack(*(values[field] for field in field_names))
    if len(body) + TYPED_HEADER_BYTES != size:
        raise AssertionError("generated typed size mismatch")
    return TYPED_HEADER_STRUCT.pack(code, size // 4, flags) + body


def pack_data_extension(name, flags=0, **values):
    return _pack_typed(DATA_EXTENSION_DEFINITIONS, name, flags, values)


def pack_control_item(name, flags=0, **values):
    return _pack_typed(CONTROL_ITEM_DEFINITIONS, name, flags, values)


def _unpack_typed(data, offset, limit, definitions, label):
    if offset + TYPED_HEADER_BYTES > limit:
        raise ProtocolError("truncated %%s header" %% label)
    type_code, length_words, flags = TYPED_HEADER_STRUCT.unpack_from(data, offset)
    size = length_words * 4
    if size < TYPED_HEADER_BYTES or offset + size > limit:
        raise ProtocolError("invalid %%s length" %% label)
    raw = bytes(data[offset:offset + size])
    definition = definitions.get(type_code)
    if definition is None:
        return {
            "name": None,
            "type": type_code,
            "flags": flags,
            "size_bytes": size,
            "raw": raw,
        }, offset + size
    name, expected_size, body_struct, field_names = definition
    if size != expected_size:
        raise ProtocolError("%%s %%s has size %%d, expected %%d" %% (label, name, size, expected_size))
    body_values = body_struct.unpack_from(raw, TYPED_HEADER_BYTES)
    return {
        "name": name,
        "type": type_code,
        "flags": flags,
        "size_bytes": size,
        "values": dict(zip(field_names, body_values)),
        "raw": raw,
    }, offset + size


def _parse_typed_sequence(data, start, limit, count, definitions, label):
    values = []
    offset = start
    previous_type = -1
    for _ in range(count):
        value, offset = _unpack_typed(data, offset, limit, definitions, label)
        if value["type"] <= previous_type:
            raise ProtocolError("%%s values must have unique ascending type codes" %% label)
        previous_type = value["type"]
        values.append(value)
    if offset != limit:
        raise ProtocolError("%%s count does not consume declared region" %% label)
    return tuple(values)


def pack_data_packet(*, packet_type, sample_format, flags, stream_id, sequence_number,
                     sample_timestamp, sample_count, channel_mask,
                     configuration_revision, calibration_revision,
                     discontinuity_revision, device_state_revision,
                     extensions=(), payload=b""):
    extensions = tuple(bytes(value) for value in extensions)
    extension_bytes = b"".join(extensions)
    header_bytes = DATA_HEADER_BYTES + len(extension_bytes)
    if header_bytes %% 4 or header_bytes > 1020:
        raise ProtocolError("data header must be four-byte aligned and fit header_words")
    header_values = (
        DATA_MAGIC, PROTOCOL_VERSION, header_bytes // 4, int(packet_type), int(sample_format),
        int(flags), stream_id, sequence_number, sample_timestamp, sample_count, channel_mask,
        len(extensions), len(payload), configuration_revision, calibration_revision,
        discontinuity_revision, device_state_revision, 0,
    )
    base = DATA_HEADER_STRUCT.pack(*header_values)
    header = bytearray(base + extension_bytes)
    checksum = crc32c(header)
    struct.pack_into("<I", header, 60, checksum)
    packet = bytes(header) + bytes(payload)
    unpack_data_packet(packet)
    return packet


def unpack_data_packet(packet):
    packet = bytes(packet)
    if len(packet) < DATA_HEADER_BYTES:
        raise ProtocolError("truncated data header")
    unpacked = DATA_HEADER_STRUCT.unpack_from(packet)
    header = dict(zip(DATA_HEADER_FIELDS, unpacked))
    if header["magic"] != DATA_MAGIC or header["protocol_version"] != PROTOCOL_VERSION:
        raise ProtocolError("unsupported data magic or version")
    header_bytes = header["header_words"] * 4
    if header_bytes < DATA_HEADER_BYTES or header_bytes > len(packet):
        raise ProtocolError("invalid data header_words")
    if header_bytes + header["payload_length"] != len(packet):
        raise ProtocolError("data payload_length does not match packet")
    checked_header = bytearray(packet[:header_bytes])
    struct.pack_into("<I", checked_header, 60, 0)
    if crc32c(checked_header) != header["header_crc32c"]:
        raise ProtocolError("data header CRC-32C mismatch")
    extensions = _parse_typed_sequence(
        packet, DATA_HEADER_BYTES, header_bytes, header["extension_count"],
        DATA_EXTENSION_BY_CODE, "data extension",
    )
    payload = packet[header_bytes:]
    by_name = {value["name"]: value for value in extensions if value["name"] is not None}

    iq_packet_types = {
        int(PacketType.RAW_IQ), int(PacketType.CALIBRATED_IQ), int(PacketType.NORMALIZED_IQ),
        int(PacketType.TRIGGERED_CAPTURE),
    }
    bytes_per_complex = {
        int(SampleFormat.S16): 4,
        int(SampleFormat.S12P): 3,
        int(SampleFormat.S8): 2,
        int(SampleFormat.S8BF): 2,
    }
    if header["packet_type"] in iq_packet_types:
        try:
            stride = bytes_per_complex[header["sample_format"]]
        except KeyError as error:
            raise ProtocolError("IQ packet uses a non-IQ sample format") from error
        expected = header["sample_count"] * bin(int(header["channel_mask"])).count("1") * stride
        if expected != len(payload):
            raise ProtocolError("IQ payload size does not match samples, channels, and format")
        if "RF_STATE" not in by_name:
            raise ProtocolError("IQ packets require temporally aligned RF_STATE metadata")
    if header["sample_format"] == int(SampleFormat.S8BF) and "QUANTIZATION" not in by_name:
        raise ProtocolError("S8BF requires QUANTIZATION metadata")
    sampled_packet_types = iq_packet_types | {
        int(PacketType.FFT), int(PacketType.STFT), int(PacketType.DETECTOR_EVENT),
        int(PacketType.VALIDITY_MASK), int(PacketType.DUAL_CHANNEL_PRODUCT),
    }
    if header["packet_type"] in sampled_packet_types and "RF_STATE" not in by_name:
        raise ProtocolError("sample-derived packets require temporally aligned RF_STATE metadata")
    rf_state = by_name.get("RF_STATE")
    if header["packet_type"] == int(PacketType.RAW_IQ):
        if header["calibration_revision"] != 0:
            raise ProtocolError("RAW_IQ must use calibration revision zero")
        if rf_state["values"]["sample_rate_hz"] != INTERNAL_SAMPLE_RATE_HZ:
            raise ProtocolError("RAW_IQ must remain at the native 61.44 MSPS rate")
    if (header["packet_type"] == int(PacketType.CALIBRATED_IQ) and
            header["calibration_revision"] == 0):
        raise ProtocolError("CALIBRATED_IQ requires a nonzero calibration revision")
    if (header["packet_type"] == int(PacketType.NORMALIZED_IQ) and
            rf_state["values"]["sample_rate_hz"] != EGRESS_SAMPLE_RATE_HZ):
        raise ProtocolError("NORMALIZED_IQ must use the canonical 55 MSPS egress rate")
    if rf_state and rf_state["values"]["sample_rate_hz"] == EGRESS_SAMPLE_RATE_HZ:
        resampler = by_name.get("RESAMPLER_STATE")
        if resampler is None:
            raise ProtocolError("55 MHz output requires RESAMPLER_STATE metadata")
        state = resampler["values"]
        expected_state = (
            INTERNAL_SAMPLE_RATE_HZ, EGRESS_SAMPLE_RATE_HZ,
            RESAMPLER_INTERPOLATION, RESAMPLER_DECIMATION, RESAMPLER_INTERPOLATION,
            header["sample_timestamp"],
        )
        observed_state = (
            state["input_rate_hz"], state["output_rate_hz"], state["interpolation"],
            state["decimation"], state["phase_denominator"], state["input_timestamp"],
        )
        if observed_state != expected_state or state["phase_numerator"] >= RESAMPLER_INTERPOLATION:
            raise ProtocolError("RESAMPLER_STATE does not match the canonical 1375/1536 model")
    if header["packet_type"] == int(PacketType.STATE_CHANGE):
        state_change = by_name.get("STATE_CHANGE")
        if (header["sample_count"] or payload or state_change is None or
                not header["flags"] & int(DataFlag.STATE_CHANGE)):
            raise ProtocolError("STATE_CHANGE packet must contain metadata and no samples")
        if state_change["values"]["activation_timestamp"] != header["sample_timestamp"]:
            raise ProtocolError("STATE_CHANGE activation timestamp mismatch")
        if (state_change["values"]["new_configuration_revision"] != header["configuration_revision"] or
                state_change["values"]["new_calibration_revision"] != header["calibration_revision"]):
            raise ProtocolError("STATE_CHANGE revision mismatch")
    product_requirements = {
        int(PacketType.FFT): "FFT_METADATA",
        int(PacketType.STFT): "STFT_METADATA",
        int(PacketType.DETECTOR_EVENT): "DETECTOR_EVENT",
        int(PacketType.TRIGGERED_CAPTURE): "TRIGGER_CAPTURE",
        int(PacketType.STATUS_SNAPSHOT): "STATUS_SNAPSHOT",
        int(PacketType.VALIDITY_MASK): "VALIDITY_MASK",
        int(PacketType.DUAL_CHANNEL_PRODUCT): "DUAL_CHANNEL_PRODUCT",
    }
    required_extension = product_requirements.get(header["packet_type"])
    if required_extension is not None and required_extension not in by_name:
        raise ProtocolError("packet type requires %%s metadata" %% required_extension)
    derived_payload_types = {
        int(PacketType.FFT), int(PacketType.STFT), int(PacketType.DETECTOR_EVENT),
        int(PacketType.STATUS_SNAPSHOT), int(PacketType.VALIDITY_MASK),
        int(PacketType.DUAL_CHANNEL_PRODUCT), int(PacketType.STATE_CHANGE),
        int(PacketType.DISCONTINUITY),
    }
    if (header["packet_type"] in derived_payload_types and
            header["sample_format"] != int(SampleFormat.NONE)):
        raise ProtocolError("non-IQ packet type requires SampleFormat.NONE")
    encoding_widths = {
        int(ProductEncoding.COMPLEX_I16): 4,
        int(ProductEncoding.POWER_U32): 4,
        int(ProductEncoding.LOG_POWER_I16_Q8_8): 2,
        int(ProductEncoding.COMPACT_TILE_U8): 1,
        int(ProductEncoding.CROSS_COMPLEX_I32): 8,
    }
    if header["packet_type"] == int(PacketType.FFT):
        metadata = by_name["FFT_METADATA"]["values"]
        width = encoding_widths.get(metadata["encoding"])
        channels = bin(int(header["channel_mask"])).count("1")
        if (width is None or metadata["fft_size"] == 0 or metadata["bin_count"] == 0 or
                header["sample_count"] != metadata["fft_size"] or
                len(payload) != metadata["bin_count"] * channels * width):
            raise ProtocolError("FFT payload does not match FFT_METADATA")
    if header["packet_type"] == int(PacketType.STFT):
        metadata = by_name["STFT_METADATA"]["values"]
        width = encoding_widths.get(metadata["encoding"])
        channels = bin(int(header["channel_mask"])).count("1")
        span = metadata["fft_size"] + ((metadata["frame_count"] - 1) * metadata["hop_size"]
                                      if metadata["frame_count"] else 0)
        if (width is None or metadata["fft_size"] == 0 or metadata["hop_size"] == 0 or
                metadata["frame_count"] == 0 or metadata["bin_count"] == 0 or
                header["sample_count"] != span or
                metadata["first_frame_timestamp"] != header["sample_timestamp"] or
                len(payload) != metadata["frame_count"] * metadata["bin_count"] * channels * width):
            raise ProtocolError("STFT payload does not match STFT_METADATA")
    if header["packet_type"] == int(PacketType.DETECTOR_EVENT):
        metadata = by_name["DETECTOR_EVENT"]["values"]
        if payload or header["sample_count"] != 0 or metadata["start_timestamp"] != header["sample_timestamp"]:
            raise ProtocolError("DETECTOR_EVENT must be payload-free and timestamp-aligned")
    if header["packet_type"] == int(PacketType.TRIGGERED_CAPTURE):
        metadata = by_name["TRIGGER_CAPTURE"]["values"]
        if (metadata["segment_count"] == 0 or metadata["segment_index"] >= metadata["segment_count"] or
                metadata["capture_format"] != header["sample_format"] or
                metadata["capture_channel_mask"] != header["channel_mask"] or
                metadata["first_timestamp"] != header["sample_timestamp"]):
            raise ProtocolError("TRIGGERED_CAPTURE metadata does not match the IQ segment")
    if header["packet_type"] == int(PacketType.STATUS_SNAPSHOT):
        if payload or header["sample_count"] != 0:
            raise ProtocolError("STATUS_SNAPSHOT must be payload-free")
    if header["packet_type"] == int(PacketType.VALIDITY_MASK):
        metadata = by_name["VALIDITY_MASK"]["values"]
        bit_count = header["sample_count"] * bin(int(header["channel_mask"])).count("1")
        if (metadata["encoding"] != int(ProductEncoding.VALIDITY_BITSET_LSB0) or
                metadata["valid_bit_value"] != 1 or metadata["bit_count"] != bit_count or
                metadata["valid_sample_count"] + metadata["invalid_sample_count"] != bit_count or
                len(payload) != (bit_count + 7) // 8):
            raise ProtocolError("VALIDITY_MASK payload does not match its metadata")
    if header["packet_type"] == int(PacketType.DUAL_CHANNEL_PRODUCT):
        metadata = by_name["DUAL_CHANNEL_PRODUCT"]["values"]
        width = encoding_widths.get(metadata["encoding"])
        if (header["channel_mask"] != 3 or width is None or metadata["element_count"] == 0 or
                metadata["reference_timestamp"] != header["sample_timestamp"] or
                len(payload) != metadata["element_count"] * width):
            raise ProtocolError("DUAL_CHANNEL_PRODUCT payload does not match its metadata")
    if header["packet_type"] == int(PacketType.DISCONTINUITY) and (payload or header["sample_count"]):
        raise ProtocolError("DISCONTINUITY packet must be payload-free")
    if header["flags"] & int(DataFlag.DISCONTINUITY) and "DISCONTINUITY" not in by_name:
        raise ProtocolError("DISCONTINUITY flag requires DISCONTINUITY metadata")
    payload_crc = by_name.get("PAYLOAD_CRC")
    if header["flags"] & int(DataFlag.PAYLOAD_CRC_PRESENT):
        if payload_crc is None or payload_crc["values"]["payload_crc32c"] != crc32c(payload):
            raise ProtocolError("payload CRC metadata is absent or invalid")
    return {"header": header, "extensions": extensions, "payload": payload}


def pack_control_message(*, message_kind, flags, command_id, status, transaction_id,
                         configuration_revision, activation_timestamp, items=()):
    items = tuple(bytes(value) for value in items)
    payload = b"".join(items)
    values = (
        CONTROL_MAGIC, PROTOCOL_VERSION, CONTROL_HEADER_BYTES // 4, int(message_kind), int(flags),
        int(command_id), int(status), transaction_id, len(payload), configuration_revision,
        activation_timestamp, crc32c(payload), 0,
    )
    header = bytearray(CONTROL_HEADER_STRUCT.pack(*values))
    struct.pack_into("<I", header, 36, crc32c(header))
    message = bytes(header) + payload
    unpack_control_message(message)
    return message


def unpack_control_message(message):
    message = bytes(message)
    if len(message) < CONTROL_HEADER_BYTES:
        raise ProtocolError("truncated control header")
    unpacked = CONTROL_HEADER_STRUCT.unpack_from(message)
    header = dict(zip(CONTROL_HEADER_FIELDS, unpacked))
    if header["magic"] != CONTROL_MAGIC or header["protocol_version"] != PROTOCOL_VERSION:
        raise ProtocolError("unsupported control magic or version")
    if header["header_words"] * 4 != CONTROL_HEADER_BYTES:
        raise ProtocolError("invalid v1 control header_words")
    if CONTROL_HEADER_BYTES + header["payload_length"] != len(message):
        raise ProtocolError("control payload_length does not match message")
    checked_header = bytearray(message[:CONTROL_HEADER_BYTES])
    struct.pack_into("<I", checked_header, 36, 0)
    if crc32c(checked_header) != header["header_crc32c"]:
        raise ProtocolError("control header CRC-32C mismatch")
    payload = message[CONTROL_HEADER_BYTES:]
    if crc32c(payload) != header["payload_crc32c"]:
        raise ProtocolError("control payload CRC-32C mismatch")
    items = []
    offset = CONTROL_HEADER_BYTES
    previous_type = -1
    while offset < len(message):
        item, offset = _unpack_typed(
            message, offset, len(message), CONTROL_ITEM_BY_CODE, "control item"
        )
        if item["type"] <= previous_type:
            raise ProtocolError("control items must have unique ascending type codes")
        previous_type = item["type"]
        items.append(item)
    by_name = {value["name"]: value for value in items if value["name"] is not None}
    for item in items:
        if item["name"] is None and item["flags"] & int(ItemFlag.REQUIRED):
            raise ProtocolError("unknown required control item")
    if header["message_kind"] == int(MessageKind.REQUEST):
        if header["status"] != int(Status.OK) or header["transaction_id"] == 0:
            raise ProtocolError("requests require status OK and a nonzero transaction_id")
        if header["flags"] & int(ControlFlag.ATOMIC) and "ATOMIC_COMMIT" not in by_name:
            raise ProtocolError("ATOMIC request requires ATOMIC_COMMIT")
    try:
        contract = CONTROL_COMMAND_CONTRACTS[header["command_id"]]
    except KeyError:
        if not (header["message_kind"] == int(MessageKind.RESPONSE) and
                header["status"] != int(Status.OK)):
            raise ProtocolError("unknown control command")
        contract = None
    known_items = tuple(item["name"] for item in items if item["name"] is not None)
    if header["message_kind"] == int(MessageKind.REQUEST):
        if contract is None or known_items != contract["request"]:
            raise ProtocolError("request items do not match the command contract")
    elif header["message_kind"] == int(MessageKind.RESPONSE):
        if header["transaction_id"] == 0:
            raise ProtocolError("responses require a nonzero transaction_id")
        if header["status"] == int(Status.OK):
            if contract is None or known_items != contract["response"]:
                raise ProtocolError("success response items do not match the command contract")
            if header["flags"] & int(ControlFlag.ERROR_DETAIL_PRESENT):
                raise ProtocolError("successful response cannot carry ERROR_DETAIL_PRESENT")
        elif known_items != ("ERROR_DETAIL",):
            raise ProtocolError("error response must carry exactly one ERROR_DETAIL item")
        elif not header["flags"] & int(ControlFlag.ERROR_DETAIL_PRESENT):
            raise ProtocolError("error response is missing ERROR_DETAIL_PRESENT")
    elif header["message_kind"] == int(MessageKind.EVENT):
        if (header["status"] != int(Status.OK) or header["transaction_id"] != 0 or
                contract is None or known_items != contract["response"]):
            raise ProtocolError("event header or items do not match the command contract")
    else:
        raise ProtocolError("unknown control message kind")
    rf_config = by_name.get("RF_CONFIG")
    if rf_config is not None:
        values = rf_config["values"]
        if (values["internal_sample_rate_hz"] != INTERNAL_SAMPLE_RATE_HZ or
                values["egress_sample_rate_hz"] != EGRESS_SAMPLE_RATE_HZ):
            raise ProtocolError("RF_CONFIG must select the canonical 61.44/55 MSPS rates")
        if values["expected_configuration_revision"] != header["configuration_revision"]:
            raise ProtocolError("RF_CONFIG expected revision does not match the control header")
    atomic = by_name.get("ATOMIC_COMMIT")
    if atomic is not None:
        values = atomic["values"]
        if (values["expected_configuration_revision"] != header["configuration_revision"] or
                values["activate_at_timestamp"] != header["activation_timestamp"]):
            raise ProtocolError("ATOMIC_COMMIT does not match the control header")
    state_change = by_name.get("STATE_CHANGE")
    if state_change is not None:
        values = state_change["values"]
        if (values["new_configuration_revision"] != header["configuration_revision"] or
                values["activation_timestamp"] != header["activation_timestamp"]):
            raise ProtocolError("control STATE_CHANGE does not match the control header")
    for chunk_name in ("CALIBRATION_CHUNK", "UPDATE_CHUNK"):
        chunk = by_name.get(chunk_name)
        if chunk is not None:
            values = chunk["values"]
            length = values["chunk_length"]
            content = values["data"]
            if (length > len(content) or values["chunk_offset"] + length > values["total_length"] or
                    any(content[length:]) or crc32c(content[:length]) != values["chunk_crc32c"]):
                raise ProtocolError("%%s length, bounds, padding, or CRC is invalid" %% chunk_name)
    return {"header": header, "items": tuple(items), "payload": payload}
''' % (
        data_sha,
        control_sha,
        "\n".join(enum_blocks).rstrip(),
        _struct_format(data["base_header"]["fields"]),
        _struct_format(control["base_header"]["fields"]),
        data_fields,
        control_fields,
        data_extensions,
        control_items,
        control_contracts,
    )


def _c_field(field: Mapping[str, Any]) -> str:
    c_type = TYPE_INFO[field["type"]][2]
    if field["type"].startswith("bytes"):
        return "    %s %s[%d];" % (c_type, field["name"], TYPE_INFO[field["type"]][0])
    return "    %s %s;" % (c_type, field["name"])


def _c_struct(
    name: str,
    fields: Sequence[Mapping[str, Any]],
    size: int,
    prefix_fields: Sequence[Tuple[str, str, int]] = (),
) -> str:
    lines = ["typedef struct %s {" % name]
    for c_type, field_name, _offset in prefix_fields:
        lines.append("    %s %s;" % (c_type, field_name))
    lines.extend(_c_field(field) for field in fields)
    lines.append("} %s;" % name)
    lines.append('_Static_assert(sizeof(%s) == %d, "%s size");' % (name, size, name))
    offsets = list(prefix_fields) + [
        (TYPE_INFO[field["type"]][2], field["name"], field["offset"]) for field in fields
    ]
    for _c_type, field_name, offset in offsets:
        lines.append(
            '_Static_assert(offsetof(%s, %s) == %d, "%s.%s offset");'
            % (name, field_name, offset, name, field_name)
        )
    return "\n".join(lines)


def _render_c(data: Mapping[str, Any], control: Mapping[str, Any]) -> str:
    data_sha = _sha256(DATA_SPEC_PATH.read_bytes())
    control_sha = _sha256(CONTROL_SPEC_PATH.read_bytes())
    enum_lines: List[str] = []
    for enum_groups in (data["enums"], control["enums"]):
        for family, values in enum_groups.items():
            family_name = _constant_name(family)
            enum_lines.append("enum neptune_edge_%s {" % family.lower())
            for name, value in values.items():
                enum_lines.append(
                    "    NEPTUNE_EDGE_%s_%s = %d," % (family_name, _constant_name(name), value)
                )
            enum_lines.append("};")
            enum_lines.append("")

    struct_blocks = [
        _c_struct(
            "neptune_edge_data_header_v1",
            data["base_header"]["fields"],
            data["base_header"]["size_bytes"],
        ),
        _c_struct(
            "neptune_edge_control_header_v1",
            control["base_header"]["fields"],
            control["base_header"]["size_bytes"],
        ),
    ]
    typed_prefix = (
        ("uint16_t", "extension_type", 0),
        ("uint16_t", "length_words", 2),
        ("uint32_t", "extension_flags", 4),
    )
    for name, definition in sorted(data["extensions"].items(), key=lambda item: item[1]["code"]):
        struct_blocks.append(
            _c_struct(
                "neptune_edge_data_%s_v1" % name.lower(),
                definition["fields"],
                definition["size_bytes"],
                typed_prefix,
            )
        )
    item_prefix = (
        ("uint16_t", "item_type", 0),
        ("uint16_t", "length_words", 2),
        ("uint32_t", "item_flags", 4),
    )
    for name, definition in sorted(control["items"].items(), key=lambda item: item[1]["code"]):
        struct_blocks.append(
            _c_struct(
                "neptune_edge_control_%s_v1" % name.lower(),
                definition["fields"],
                definition["size_bytes"],
                item_prefix,
            )
        )

    data_extension_codes = "\n".join(
        "#define NEPTUNE_EDGE_DATA_EXTENSION_%s UINT16_C(%d)"
        % (_constant_name(name), definition["code"])
        for name, definition in sorted(data["extensions"].items(), key=lambda item: item[1]["code"])
    )
    control_item_codes = "\n".join(
        "#define NEPTUNE_EDGE_CONTROL_ITEM_%s UINT16_C(%d)"
        % (_constant_name(name), definition["code"])
        for name, definition in sorted(control["items"].items(), key=lambda item: item[1]["code"])
    )
    return '''/* Generated by scripts/generate_protocol.py; DO NOT EDIT. */
#ifndef NEPTUNE_EDGE_V1_H
#define NEPTUNE_EDGE_V1_H

#include <stddef.h>
#include <stdint.h>

#define NEPTUNE_EDGE_DATA_SPEC_SHA256 "%s"
#define NEPTUNE_EDGE_CONTROL_SPEC_SHA256 "%s"
#define NEPTUNE_EDGE_PROTOCOL_VERSION UINT8_C(1)
#define NEPTUNE_EDGE_DATA_MAGIC_LE UINT32_C(0x5044454e)
#define NEPTUNE_EDGE_CONTROL_MAGIC_LE UINT32_C(0x5043454e)
#define NEPTUNE_EDGE_DATA_HEADER_BYTES UINT16_C(64)
#define NEPTUNE_EDGE_CONTROL_HEADER_BYTES UINT16_C(40)
#define NEPTUNE_EDGE_TYPED_HEADER_BYTES UINT16_C(8)
#define NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ UINT32_C(61440000)
#define NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ UINT32_C(55000000)
#define NEPTUNE_EDGE_RESAMPLER_INTERPOLATION UINT16_C(1375)
#define NEPTUNE_EDGE_RESAMPLER_DECIMATION UINT16_C(1536)

%s
%s
%s
#pragma pack(push, 1)
%s
#pragma pack(pop)

static inline uint32_t neptune_edge_crc32c(const void *buffer, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
    uint32_t crc = UINT32_C(0xffffffff);
    size_t index;
    for (index = 0; index < length; ++index) {
        unsigned bit;
        crc ^= bytes[index];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & UINT32_C(1)) ? UINT32_C(0x82f63b78) : UINT32_C(0));
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

static inline int8_t neptune_edge_quantize_s8(int16_t value, unsigned shift)
{
    int32_t magnitude;
    int32_t quotient;
    int32_t remainder;
    int32_t halfway;
    int32_t rounded;
    if (shift == 0U) {
        rounded = value;
    } else if (shift >= 31U) {
        rounded = 0;
    } else {
        magnitude = value < 0 ? -(int32_t)value : (int32_t)value;
        quotient = magnitude >> shift;
        remainder = magnitude & (((int32_t)1 << shift) - 1);
        halfway = (int32_t)1 << (shift - 1U);
        if (remainder > halfway || (remainder == halfway && (quotient & 1) != 0)) {
            ++quotient;
        }
        rounded = value < 0 ? -quotient : quotient;
    }
    if (rounded > 127) {
        return INT8_C(127);
    }
    if (rounded < -128) {
        return -INT8_C(127) - INT8_C(1);
    }
    return (int8_t)rounded;
}

static inline void neptune_edge_pack_s12p(uint8_t output[3], int16_t i_value, int16_t q_value)
{
    uint32_t word = ((uint16_t)i_value & UINT32_C(0x0fff)) |
        (((uint32_t)(uint16_t)q_value & UINT32_C(0x0fff)) << 12);
    output[0] = (uint8_t)word;
    output[1] = (uint8_t)(word >> 8);
    output[2] = (uint8_t)(word >> 16);
}

static inline int16_t neptune_edge_sign_extend_s12(uint16_t value)
{
    value &= UINT16_C(0x0fff);
    return (value & UINT16_C(0x0800)) != 0U ?
        (int16_t)(value | UINT16_C(0xf000)) : (int16_t)value;
}

static inline void neptune_edge_unpack_s12p(
    const uint8_t input[3], int16_t *i_value, int16_t *q_value)
{
    uint32_t word = (uint32_t)input[0] | ((uint32_t)input[1] << 8) |
        ((uint32_t)input[2] << 16);
    *i_value = neptune_edge_sign_extend_s12((uint16_t)word);
    *q_value = neptune_edge_sign_extend_s12((uint16_t)(word >> 12));
}

static inline uint64_t neptune_edge_source_tick(
    uint64_t input_timestamp, uint16_t phase_numerator, uint32_t output_index)
{
    return input_timestamp +
        ((uint64_t)phase_numerator + (uint64_t)output_index * NEPTUNE_EDGE_RESAMPLER_DECIMATION) /
        NEPTUNE_EDGE_RESAMPLER_INTERPOLATION;
}

#endif /* NEPTUNE_EDGE_V1_H */
''' % (
        data_sha,
        control_sha,
        "\n".join(enum_lines).rstrip(),
        data_extension_codes,
        control_item_codes,
        "\n\n".join(struct_blocks),
    )


def _hex_vector(name: str, kind: str, description: str, wire: bytes, expected: Mapping[str, Any]) -> Dict[str, Any]:
    return {
        "name": name,
        "kind": kind,
        "description": description,
        "bytes": len(wire),
        "sha256": _sha256(wire),
        "wire_hex": wire.hex(),
        "expected": dict(expected),
    }


def _render_golden(python_source: str) -> str:
    namespace: Dict[str, Any] = {"__name__": "neptune_edge_v1_generated_for_vectors"}
    exec(compile(python_source, str(PYTHON_OUTPUT), "exec"), namespace)
    pack_data_extension = namespace["pack_data_extension"]
    pack_data_packet = namespace["pack_data_packet"]
    pack_control_item = namespace["pack_control_item"]
    pack_control_message = namespace["pack_control_message"]
    pack_iq_samples = namespace["pack_iq_samples"]
    crc32c = namespace["crc32c"]

    timestamp = 0x0000000012345678
    payload = bytes.fromhex("7f80fe020102fefd1020f0e055aaab54")
    data_extensions = (
        pack_data_extension(
            "RF_STATE",
            center_frequency_hz=2_450_000_000,
            sample_rate_hz=55_000_000,
            rf_bandwidth_hz=50_000_000,
            rx1_gain_mdb=30_000,
            rx2_gain_mdb=29_500,
            digital_gain_q16_16=65_536,
            temperature_mc=42_125,
            rx1_gain_mode=0,
            rx2_gain_mode=0,
            pll_lock_mask=3,
            channel_mask=3,
            device_flags=0,
        ),
        pack_data_extension(
            "QUANTIZATION",
            exponent=4,
            headroom_bits=1,
            strategy=1,
            rounding=1,
            scale_numerator=1,
            scale_denominator=16,
            block_rms_q16_16=0x00322000,
            block_peak_q16_16=0x007F0000,
            clipping_count=1,
            valid_sample_count=8,
            invalid_sample_count=0,
        ),
        pack_data_extension(
            "RESAMPLER_STATE",
            input_rate_hz=61_440_000,
            output_rate_hz=55_000_000,
            interpolation=1375,
            decimation=1536,
            phase_numerator=123,
            phase_denominator=1375,
            input_timestamp=timestamp,
            output_sample_index=0x0000000000000ABC,
        ),
        pack_data_extension(
            "BLOCK_STATS",
            rms_q16_16=0x00322000,
            peak_q16_16=0x007F0000,
            clipping_count=1,
            valid_sample_count=8,
            invalid_sample_count=0,
            reserved=0,
        ),
        pack_data_extension("PAYLOAD_CRC", payload_crc32c=crc32c(payload), reserved=0),
    )
    data_wire = pack_data_packet(
        packet_type=3,
        sample_format=4,
        flags=128 | 256,
        stream_id=0x01020304,
        sequence_number=0x0102030405060708,
        sample_timestamp=timestamp,
        sample_count=4,
        channel_mask=3,
        configuration_revision=7,
        calibration_revision=3,
        discontinuity_revision=0,
        device_state_revision=11,
        extensions=data_extensions,
        payload=payload,
    )

    activation = timestamp + 61_440
    state_extension = pack_data_extension(
        "STATE_CHANGE",
        change_kind=1,
        scope=3,
        reserved=0,
        changed_fields=1 | 8,
        previous_configuration_revision=7,
        new_configuration_revision=8,
        previous_calibration_revision=3,
        new_calibration_revision=3,
        activation_timestamp=activation,
    )
    state_wire = pack_data_packet(
        packet_type=9,
        sample_format=0,
        flags=64 | 128,
        stream_id=0,
        sequence_number=9,
        sample_timestamp=activation,
        sample_count=0,
        channel_mask=3,
        configuration_revision=8,
        calibration_revision=3,
        discontinuity_revision=0,
        device_state_revision=12,
        extensions=(state_extension,),
        payload=b"",
    )

    discontinuity_extension = pack_data_extension(
        "DISCONTINUITY",
        reason=3,
        action=2,
        lost_input_samples=0xFFFFFFFF,
        last_good_timestamp=activation + 100,
        first_valid_timestamp=activation + 4096,
    )
    discontinuity_wire = pack_data_packet(
        packet_type=10,
        sample_format=0,
        flags=1 | 32 | 128,
        stream_id=0x01020304,
        sequence_number=10,
        sample_timestamp=activation + 4096,
        sample_count=0,
        channel_mask=3,
        configuration_revision=8,
        calibration_revision=3,
        discontinuity_revision=1,
        device_state_revision=12,
        extensions=(discontinuity_extension,),
        payload=b"",
    )

    request_items = (
        pack_control_item(
            "RF_CONFIG",
            flags=1,
            center_frequency_hz=2_450_000_000,
            internal_sample_rate_hz=61_440_000,
            egress_sample_rate_hz=55_000_000,
            rf_bandwidth_hz=50_000_000,
            rx1_gain_mdb=30_000,
            rx2_gain_mdb=29_500,
            channel_mask=3,
            rx1_gain_mode=0,
            rx2_gain_mode=0,
            expected_configuration_revision=7,
            rf_flags=0,
        ),
        pack_control_item(
            "ATOMIC_COMMIT",
            flags=1,
            expected_configuration_revision=7,
            expected_calibration_revision=3,
            activate_at_timestamp=activation,
            changed_fields=1 | 8,
        ),
    )
    request_wire = pack_control_message(
        message_kind=1,
        flags=1 | 2 | 4,
        command_id=256,
        status=0,
        transaction_id=0xA1B2C3D4,
        configuration_revision=7,
        activation_timestamp=activation,
        items=request_items,
    )

    response_item = pack_control_item(
        "STATE_CHANGE",
        previous_configuration_revision=7,
        new_configuration_revision=8,
        previous_calibration_revision=3,
        new_calibration_revision=3,
        activation_timestamp=activation,
        changed_fields=1 | 8,
    )
    response_wire = pack_control_message(
        message_kind=2,
        flags=4,
        command_id=256,
        status=0,
        transaction_id=0xA1B2C3D4,
        configuration_revision=8,
        activation_timestamp=activation,
        items=(response_item,),
    )

    native_iq = ((-2048, 2047), (-1, 0), (1, -1), (1024, -1024))
    compact_iq = ((-128, 127), (-1, 0), (1, -1), (64, -64))
    s16_payload = pack_iq_samples(1, native_iq)
    s12p_payload = pack_iq_samples(2, native_iq)
    s8_payload = pack_iq_samples(3, compact_iq)

    value = {
        "schema": "neptunesdr.edge.golden-vectors/v1",
        "generator": "scripts/generate_protocol.py",
        "data_spec_sha256": _sha256(DATA_SPEC_PATH.read_bytes()),
        "control_spec_sha256": _sha256(CONTROL_SPEC_PATH.read_bytes()),
        "vectors": [
            _hex_vector(
                "normalized_s8bf_dual_rx_55msps",
                "data",
                "Dual-RX S8BF packet with exact resampler state, measurement metadata, and payload CRC.",
                data_wire,
                {
                    "packet_type": "NORMALIZED_IQ",
                    "sample_format": "S8BF",
                    "stream_id": 0x01020304,
                    "sequence_number": 0x0102030405060708,
                    "sample_timestamp": timestamp,
                    "sample_count": 4,
                    "channel_mask": 3,
                    "configuration_revision": 7,
                    "calibration_revision": 3,
                    "resampler_phase_numerator": 123,
                },
            ),
            _hex_vector(
                "configuration_state_change",
                "data",
                "Payload-free state change activated on an exact ingress sample tick.",
                state_wire,
                {
                    "packet_type": "STATE_CHANGE",
                    "activation_timestamp": activation,
                    "previous_configuration_revision": 7,
                    "new_configuration_revision": 8,
                },
            ),
            _hex_vector(
                "dma_overrun_discontinuity",
                "data",
                "Observable DMA overrun with unknown loss count and a new continuity epoch.",
                discontinuity_wire,
                {
                    "packet_type": "DISCONTINUITY",
                    "reason": "DMA_OVERRUN",
                    "discontinuity_revision": 1,
                    "lost_input_samples": 0xFFFFFFFF,
                },
            ),
            _hex_vector(
                "atomic_set_rf_request",
                "control",
                "Atomic USB SET_RF request pinned to expected revisions and an activation tick.",
                request_wire,
                {
                    "message_kind": "REQUEST",
                    "command": "SET_RF",
                    "transaction_id": 0xA1B2C3D4,
                    "configuration_revision": 7,
                    "activation_timestamp": activation,
                },
            ),
            _hex_vector(
                "atomic_set_rf_response",
                "control",
                "Successful SET_RF response carrying the accepted revision transition.",
                response_wire,
                {
                    "message_kind": "RESPONSE",
                    "command": "SET_RF",
                    "transaction_id": 0xA1B2C3D4,
                    "configuration_revision": 8,
                    "activation_timestamp": activation,
                },
            ),
            _hex_vector(
                "iq_payload_s16",
                "iq-payload",
                "Native signed 12-bit ADC codes sign-extended in S16 I/Q containers.",
                s16_payload,
                {"sample_format": "S16", "samples": [list(pair) for pair in native_iq]},
            ),
            _hex_vector(
                "iq_payload_s12p",
                "iq-payload",
                "The same native ADC codes losslessly packed into three bytes per complex sample.",
                s12p_payload,
                {"sample_format": "S12P", "samples": [list(pair) for pair in native_iq]},
            ),
            _hex_vector(
                "iq_payload_s8",
                "iq-payload",
                "Signed int8 I/Q payload after deterministic rounding and saturation.",
                s8_payload,
                {"sample_format": "S8", "samples": [list(pair) for pair in compact_iq]},
            ),
        ],
    }
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def _outputs() -> Mapping[Path, bytes]:
    data = _json(DATA_SPEC_PATH)
    control = _json(CONTROL_SPEC_PATH)
    validate_specs(data, control)
    python_source = _render_python(data, control)
    c_source = _render_c(data, control)
    golden = _render_golden(python_source)
    return {
        PYTHON_OUTPUT: python_source.encode("utf-8"),
        C_OUTPUT: c_source.encode("utf-8"),
        GOLDEN_OUTPUT: golden.encode("utf-8"),
    }


def _check(outputs: Mapping[Path, bytes]) -> int:
    stale = []
    for path, expected in outputs.items():
        try:
            observed = path.read_bytes()
        except FileNotFoundError:
            observed = None
        if observed != expected:
            stale.append(path.relative_to(ROOT).as_posix())
    if stale:
        print("generated protocol outputs are missing or stale:", file=sys.stderr)
        for path in stale:
            print("  %s" % path, file=sys.stderr)
        print("run: python3 scripts/generate_protocol.py", file=sys.stderr)
        return 1
    print("NEPTUNE_EDGE_V1_GENERATED PASS")
    return 0


def _write(outputs: Mapping[Path, bytes]) -> int:
    for path, content in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        print("wrote %s" % path.relative_to(ROOT))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if generated outputs differ; write nothing")
    args = parser.parse_args(argv)
    outputs = _outputs()
    return _check(outputs) if args.check else _write(outputs)


if __name__ == "__main__":
    raise SystemExit(main())
