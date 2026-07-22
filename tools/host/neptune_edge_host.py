#!/usr/bin/env python3
"""Strict host helpers for Neptune Edge v1 control and UDP messages."""

from __future__ import annotations

import os
from pathlib import Path
import socket
import struct
import sys
from typing import BinaryIO, Dict, List, Optional, Tuple


REPOSITORY = Path(__file__).resolve().parents[2]
GENERATED = REPOSITORY / "protocol" / "generated"
if str(GENERATED) not in sys.path:
    sys.path.insert(0, str(GENERATED))

import neptune_edge_v1 as edge  # noqa: E402


def decode_item(item: Dict[str, object]) -> Dict[str, object]:
    """Present canonical generated items and preserve unknown optional items."""
    if item["name"] is not None:
        decoded = {"name": item["name"], **dict(item["values"])}
        if item["name"] == "SAFETY":
            for name in (
                "tx_enabled",
                "tx_inhibited",
                "tx_default_off",
                "qspi_write_allowed",
            ):
                decoded[name] = bool(decoded[name])
        return decoded
    raw = bytes(item["raw"])[edge.TYPED_HEADER_BYTES :]
    item_type = int(item["type"])
    return {
        "name": "UNKNOWN_%04x" % item_type,
        "flags": int(item["flags"]),
        "raw_hex": raw.hex(),
    }


def decode_control(message: bytes) -> Dict[str, object]:
    decoded = edge.unpack_control_message(message)
    return {
        "header": decoded["header"],
        "items": [decode_item(item) for item in decoded["items"]],
    }


def make_request(
    command: int,
    transaction_id: int,
    configuration_revision: int,
    *,
    items: Tuple[bytes, ...] = (),
    atomic: bool = False,
    activation_timestamp: int = edge.UINT64_MAX,
) -> bytes:
    flags = int(edge.ControlFlag.ACK_REQUIRED)
    if atomic:
        flags |= int(edge.ControlFlag.ATOMIC)
        if activation_timestamp != edge.UINT64_MAX:
            flags |= int(edge.ControlFlag.SCHEDULED)
    return edge.pack_control_message(
        message_kind=edge.MessageKind.REQUEST,
        flags=flags,
        command_id=command,
        status=edge.Status.OK,
        transaction_id=transaction_id,
        configuration_revision=configuration_revision,
        activation_timestamp=activation_timestamp,
        items=items,
    )


def atomic_item(
    configuration_revision: int,
    calibration_revision: int,
    activation_timestamp: int,
    changed_fields: int,
) -> bytes:
    return edge.pack_control_item(
        "ATOMIC_COMMIT",
        flags=int(edge.ItemFlag.REQUIRED),
        expected_configuration_revision=configuration_revision,
        expected_calibration_revision=calibration_revision,
        activate_at_timestamp=activation_timestamp,
        changed_fields=changed_fields,
    )


class ControlStream:
    """Raw full-duplex framed stream used by Unix and vendor-bulk adapters."""

    def __init__(self, reader: BinaryIO, writer: BinaryIO) -> None:
        self.reader = reader
        self.writer = writer

    def close(self) -> None:
        if self.writer is not self.reader:
            self.writer.close()
        self.reader.close()

    def send(self, message: bytes) -> None:
        self.writer.write(message)
        self.writer.flush()

    def _read_exact(self, length: int) -> bytes:
        output = bytearray()
        while len(output) < length:
            value = self.reader.read(length - len(output))
            if not value:
                raise EOFError("control transport closed within a frame")
            output.extend(value)
        return bytes(output)

    def receive(self) -> bytes:
        header = self._read_exact(edge.CONTROL_HEADER_BYTES)
        if header[:4] != edge.CONTROL_MAGIC:
            raise edge.ProtocolError("control stream lost NECP framing")
        payload_length = struct.unpack_from("<I", header, 16)[0]
        if payload_length > 4096:
            raise edge.ProtocolError("control payload exceeds daemon limit")
        message = header + self._read_exact(payload_length)
        edge.unpack_control_message(message)
        return message

    def transact(self, request: bytes) -> Tuple[Dict[str, object], List[Dict[str, object]]]:
        expected = edge.unpack_control_message(request)["header"]["transaction_id"]
        self.send(request)
        events: List[Dict[str, object]] = []
        while True:
            decoded = decode_control(self.receive())
            header = decoded["header"]
            if header["message_kind"] == int(edge.MessageKind.EVENT):
                events.append(decoded)
                continue
            if header["message_kind"] != int(edge.MessageKind.RESPONSE):
                raise edge.ProtocolError("non-response received on control transaction")
            if header["transaction_id"] != expected:
                raise edge.ProtocolError("control transaction ID mismatch")
            return decoded, events


def open_unix_control(path: str) -> ControlStream:
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.connect(path)
    stream = connection.makefile("rwb", buffering=0)
    return ControlStream(stream, stream)


def open_raw_control(path: str) -> ControlStream:
    """Open an already-bound raw vendor-bulk character/relay endpoint."""

    fd = os.open(path, os.O_RDWR | getattr(os, "O_CLOEXEC", 0))
    reader = os.fdopen(os.dup(fd), "rb", buffering=0)
    writer = os.fdopen(fd, "wb", buffering=0)
    return ControlStream(reader, writer)


class PacketTracker:
    """Validate NEDP packets and track sequence plus exact sample-time state."""

    def __init__(self) -> None:
        self.streams: Dict[int, Dict[str, int]] = {}
        self.packets = 0
        self.payload_bytes = 0
        self.sequence_gaps = 0
        self.timing_breaks = 0

    def accept(self, packet: bytes) -> Dict[str, object]:
        decoded = edge.unpack_data_packet(packet)
        header = decoded["header"]
        stream_id = int(header["stream_id"])
        sequence = int(header["sequence_number"])
        state = self.streams.get(stream_id)
        gap = state is not None and sequence != state["sequence"]
        timing_break = False
        extensions = {
            item["name"]: item
            for item in decoded["extensions"]
            if item["name"] is not None
        }
        resampler = extensions.get("RESAMPLER_STATE")
        if state is not None and resampler is not None:
            values = resampler["values"]
            timing_break = (
                int(values["input_timestamp"]) != state.get("input_timestamp")
                or int(values["phase_numerator"]) != state.get("phase")
                or int(values["output_sample_index"]) != state.get("output_index")
            )
        elif state is not None and "input_timestamp" in state:
            timing_break = int(header["sample_timestamp"]) != state["input_timestamp"]
        if gap:
            self.sequence_gaps += 1
        if timing_break:
            self.timing_breaks += 1
        next_state: Dict[str, int] = {"sequence": sequence + 1}
        if resampler is not None:
            values = resampler["values"]
            next_timestamp, next_phase = edge.advance_resampler(
                int(values["input_timestamp"]),
                int(values["phase_numerator"]),
                int(header["sample_count"]),
            )
            next_state.update(
                input_timestamp=next_timestamp,
                phase=next_phase,
                output_index=int(values["output_sample_index"])
                + int(header["sample_count"]),
            )
        else:
            next_state["input_timestamp"] = int(header["sample_timestamp"]) + int(
                header["sample_count"]
            )
        self.streams[stream_id] = next_state
        self.packets += 1
        self.payload_bytes += len(decoded["payload"])
        return {
            "decoded": decoded,
            "sequence_gap": gap,
            "timing_break": timing_break,
        }
