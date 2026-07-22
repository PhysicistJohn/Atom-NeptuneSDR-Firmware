#!/usr/bin/env python3
"""Compute fail-closed Neptune Edge UDP/GigE transport budgets."""

from __future__ import annotations

import argparse
import json
import math


ETHERNET_RATE_BPS = 1_000_000_000
ETHERNET_HEADER_BYTES = 14
ETHERNET_FCS_BYTES = 4
ETHERNET_PREAMBLE_BYTES = 8
ETHERNET_IFG_BYTES = 12
IPV4_HEADER_BYTES = 20
UDP_HEADER_BYTES = 8

FORMAT_BYTES = {"S16": 4, "S12P": 3, "S8": 2, "S8BF": 2}
# Worst-case canonical IQ prefix reserved in every datagram budget. RF state,
# payload CRC, and quantization/block statistics are present; 55 MHz products
# carry exact resampler state. Space for one discontinuity extension is kept so
# a fault marker never forces IP fragmentation or a different packet profile.
HEADER_BYTES = {
    "S16": 64 + 48 + 32 + 16 + 32,
    "S12P": 64 + 48 + 32 + 16 + 32,
    "S8": 64 + 48 + 40 + 32 + 16 + 32,
    "S8BF": 64 + 48 + 40 + 40 + 32 + 16 + 32,
}


def transport_budget(sample_rate_hz: int, sample_format: str, channels: int, mtu: int) -> dict:
    if sample_rate_hz <= 0 or sample_format not in FORMAT_BYTES or channels not in (1, 2):
        raise ValueError("invalid stream rate, format, or channel count")
    if mtu < 576 or mtu > 9000:
        raise ValueError("MTU must be in the supported IPv4 range 576..9000")
    bytes_per_period = FORMAT_BYTES[sample_format] * channels
    app_header = HEADER_BYTES[sample_format]
    udp_payload_capacity = mtu - IPV4_HEADER_BYTES - UDP_HEADER_BYTES
    sample_payload_capacity = udp_payload_capacity - app_header
    samples_per_packet = sample_payload_capacity // bytes_per_period
    if samples_per_packet <= 0:
        raise ValueError("metadata does not fit selected MTU")
    sample_payload_bytes = samples_per_packet * bytes_per_period
    ip_bytes = IPV4_HEADER_BYTES + UDP_HEADER_BYTES + app_header + sample_payload_bytes
    wire_bytes = (
        ETHERNET_PREAMBLE_BYTES
        + ETHERNET_HEADER_BYTES
        + ip_bytes
        + ETHERNET_FCS_BYTES
        + ETHERNET_IFG_BYTES
    )
    packets_per_second = sample_rate_hz / samples_per_packet
    wire_rate_bps = packets_per_second * wire_bytes * 8
    payload_rate_bps = sample_rate_hz * bytes_per_period * 8
    max_sample_rate_hz = math.floor(ETHERNET_RATE_BPS * samples_per_packet / (wire_bytes * 8))
    return {
        "sample_rate_hz": sample_rate_hz,
        "sample_format": sample_format,
        "channels": channels,
        "mtu": mtu,
        "application_header_bytes": app_header,
        "samples_per_packet": samples_per_packet,
        "packets_per_second": packets_per_second,
        "payload_rate_bps": payload_rate_bps,
        "wire_rate_bps": wire_rate_bps,
        "line_rate_fraction": wire_rate_bps / ETHERNET_RATE_BPS,
        "max_sample_rate_hz_at_line_rate": max_sample_rate_hz,
        "fits_gigabit": wire_rate_bps <= ETHERNET_RATE_BPS,
        "ip_fragmentation": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sample-rate", type=int, default=55_000_000)
    parser.add_argument("--format", choices=sorted(FORMAT_BYTES), default="S8BF")
    parser.add_argument("--channels", type=int, choices=(1, 2), default=1)
    parser.add_argument("--mtu", type=int, default=9000)
    parser.add_argument("--require-fit", action="store_true")
    args = parser.parse_args()
    result = transport_budget(args.sample_rate, args.format, args.channels, args.mtu)
    print(json.dumps(result, indent=2, sort_keys=True))
    if args.require_fit and not result["fits_gigabit"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
