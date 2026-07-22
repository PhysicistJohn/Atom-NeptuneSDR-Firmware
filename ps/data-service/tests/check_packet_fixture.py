"""Validate the C packet builder through the canonical generated binding."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "protocol" / "generated"))

import neptune_edge_v1 as edge  # noqa: E402


packet = edge.unpack_data_packet(sys.stdin.buffer.read())
header = packet["header"]
assert header["packet_type"] == int(edge.PacketType.NORMALIZED_IQ)
assert header["sample_format"] == int(edge.SampleFormat.S8BF)
assert header["sample_timestamp"] == 123456
assert header["configuration_revision"] == 11
assert len(packet["payload"]) == 8
names = {value["name"] for value in packet["extensions"]}
assert {"RF_STATE", "QUANTIZATION", "RESAMPLER_STATE", "BLOCK_STATS", "PAYLOAD_CRC"} <= names
quantization = next(
    value["values"] for value in packet["extensions"] if value["name"] == "QUANTIZATION"
)
assert quantization["exponent"] == 4
assert quantization["headroom_bits"] == 1
assert quantization["strategy"] == int(edge.QuantizationStrategy.PEAK)
assert quantization["rounding"] == int(edge.RoundingMode.ROUND_TO_NEAREST_EVEN)
assert quantization["scale_numerator"] == 1
assert quantization["scale_denominator"] == 16
print("C packet fixture canonical parse: PASS")
