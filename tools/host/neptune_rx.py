#!/usr/bin/env python3
"""Strict Neptune Edge v1 UDP receiver and length-framed capture writer."""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time

from neptune_edge_host import PacketTracker, edge


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--bind", default="0.0.0.0")
    result.add_argument("--port", type=int, required=True)
    result.add_argument("--count", type=int, default=0, help="zero runs continuously")
    result.add_argument("--capture", help="exclusive-create, u32-length framed datagrams")
    result.add_argument("--receive-buffer", type=int, default=8 * 1024 * 1024)
    result.add_argument("--status-every", type=int, default=1000)
    return result


def main() -> int:
    args = parser().parse_args()
    if not (1 <= args.port <= 65535) or args.count < 0 or args.status_every < 1:
        parser().error("port/count/status interval is outside its valid range")
    tracker = PacketTracker()
    capture = None
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.receive_buffer)
        receiver.bind((args.bind, args.port))
        if args.capture:
            capture = open(args.capture, "xb")
        started = time.monotonic()
        while args.count == 0 or tracker.packets < args.count:
            packet, address = receiver.recvfrom(65535)
            try:
                result = tracker.accept(packet)
            except edge.ProtocolError as error:
                print(
                    json.dumps(
                        {
                            "event": "malformed_packet",
                            "source": "%s:%d" % address,
                            "error": str(error),
                        },
                        sort_keys=True,
                    ),
                    file=sys.stderr,
                )
                continue
            header = result["decoded"]["header"]
            if result["sequence_gap"] or result["timing_break"] or (
                int(header["flags"]) & int(edge.DataFlag.DISCONTINUITY)
            ):
                print(
                    json.dumps(
                        {
                            "event": "continuity",
                            "stream_id": header["stream_id"],
                            "sequence": header["sequence_number"],
                            "sample_timestamp": header["sample_timestamp"],
                            "sequence_gap": result["sequence_gap"],
                            "timing_break": result["timing_break"],
                            "device_marked": bool(
                                int(header["flags"])
                                & int(edge.DataFlag.DISCONTINUITY)
                            ),
                        },
                        sort_keys=True,
                    ),
                    file=sys.stderr,
                )
            if capture is not None:
                capture.write(struct.pack("<I", len(packet)))
                capture.write(packet)
            if tracker.packets % args.status_every == 0:
                elapsed = max(time.monotonic() - started, 1e-9)
                print(
                    json.dumps(
                        {
                            "packets": tracker.packets,
                            "payload_bytes": tracker.payload_bytes,
                            "sequence_gaps": tracker.sequence_gaps,
                            "timing_breaks": tracker.timing_breaks,
                            "payload_mbit_s": tracker.payload_bytes * 8 / elapsed / 1e6,
                        },
                        sort_keys=True,
                    ),
                    file=sys.stderr,
                )
    except KeyboardInterrupt:
        pass
    except OSError as error:
        print("receiver error: %s" % error, file=sys.stderr)
        return 1
    finally:
        receiver.close()
        if capture is not None:
            capture.flush()
            capture.close()
    print(
        json.dumps(
            {
                "packets": tracker.packets,
                "payload_bytes": tracker.payload_bytes,
                "sequence_gaps": tracker.sequence_gaps,
                "timing_breaks": tracker.timing_breaks,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
