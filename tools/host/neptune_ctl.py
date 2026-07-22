#!/usr/bin/env python3
"""Neptune Edge v1 host control CLI for Unix or raw bulk streams."""

from __future__ import annotations

import argparse
import ipaddress
import json
import secrets
import sys

from neptune_edge_host import (
    atomic_item,
    edge,
    make_request,
    open_raw_control,
    open_unix_control,
)


PRODUCT_BITS = {
    "raw": 1 << int(edge.PacketType.RAW_IQ),
    "calibrated": 1 << int(edge.PacketType.CALIBRATED_IQ),
    "normalized": 1 << int(edge.PacketType.NORMALIZED_IQ),
}
FORMATS = {
    "s16": edge.SampleFormat.S16,
    "s12p": edge.SampleFormat.S12P,
    "s8": edge.SampleFormat.S8,
    "s8bf": edge.SampleFormat.S8BF,
}
GAIN_MODES = {
    "manual": edge.GainMode.MANUAL,
    "slow": edge.GainMode.SLOW_ATTACK,
    "fast": edge.GainMode.FAST_ATTACK,
    "hybrid": edge.GainMode.HYBRID,
}


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    transport = result.add_mutually_exclusive_group(required=True)
    transport.add_argument("--unix", metavar="PATH", help="daemon Unix socket")
    transport.add_argument(
        "--raw-device",
        metavar="PATH",
        help="already-bound raw full-duplex bulk/relay character endpoint",
    )
    result.add_argument("--transaction-id", type=int, default=0)
    commands = result.add_subparsers(dest="command", required=True)
    for name in ("identity", "health", "get-rf", "get-counters"):
        commands.add_parser(name)
    reset = commands.add_parser("reset-counters")
    reset.add_argument("--revision", type=int, default=0)
    reset.add_argument("--reason-code", type=lambda value: int(value, 0), default=0)
    hard_disable = commands.add_parser("hard-disable-tx")
    hard_disable.add_argument("--revision", type=int, required=True)
    hard_disable.add_argument(
        "--reason-code", type=lambda value: int(value, 0), default=0
    )

    rf = commands.add_parser("set-rf")
    rf.add_argument("--revision", type=int, required=True)
    rf.add_argument("--calibration-revision", type=int, default=0)
    rf.add_argument("--frequency", type=int, required=True)
    rf.add_argument("--bandwidth", type=int, required=True)
    rf.add_argument("--rx1-gain-mdb", type=int, required=True)
    rf.add_argument("--rx2-gain-mdb", type=int, required=True)
    rf.add_argument("--channels", type=int, choices=(1, 2, 3), default=1)
    rf.add_argument("--rx1-gain-mode", choices=GAIN_MODES, default="manual")
    rf.add_argument("--rx2-gain-mode", choices=GAIN_MODES, default="manual")
    rf.add_argument("--activate-at", type=int, default=edge.UINT64_MAX)

    pipeline = commands.add_parser("configure-pipeline")
    pipeline.add_argument("--revision", type=int, required=True)
    pipeline.add_argument("--calibration-revision", type=int, default=0)
    pipeline.add_argument("--changed-fields", type=lambda value: int(value, 0), required=True)
    pipeline.add_argument("--activate-at", type=int, default=edge.UINT64_MAX)

    for name in ("create-stream", "start-stream", "stop-stream", "destroy-stream"):
        stream = commands.add_parser(name)
        stream.add_argument("--revision", type=int, required=True)
        stream.add_argument("--calibration-revision", type=int, default=0)
        stream.add_argument("--stream-id", type=int, required=True)
        stream.add_argument("--destination", required=True)
        stream.add_argument("--port", type=int, required=True)
        stream.add_argument("--mtu", type=int, choices=(1500, 9000), default=9000)
        stream.add_argument("--product", choices=PRODUCT_BITS, default="normalized")
        stream.add_argument("--format", choices=FORMATS, default="s8")
        stream.add_argument("--channels", type=int, choices=(1, 2, 3), default=1)
        stream.add_argument("--samples-per-packet", type=int, default=4096)
        stream.add_argument("--activate-at", type=int, default=edge.UINT64_MAX)
    return result


def build(args: argparse.Namespace, transaction: int) -> bytes:
    simple = {
        "identity": edge.Command.GET_IDENTITY,
        "health": edge.Command.GET_HEALTH,
        "get-rf": edge.Command.GET_RF,
        "get-counters": edge.Command.GET_COUNTERS,
    }
    if args.command in simple:
        return make_request(simple[args.command], transaction, 0)
    if args.command == "reset-counters":
        action = edge.pack_control_item(
            "SYSTEM_ACTION",
            flags=int(edge.ItemFlag.REQUIRED),
            action_kind=int(edge.SystemActionKind.RESET_COUNTERS),
            action_flags=0,
            reason_code=args.reason_code,
            authorization_token=0,
        )
        return make_request(
            edge.Command.RESET_COUNTERS,
            transaction,
            args.revision,
            items=(action,),
        )
    if args.command == "hard-disable-tx":
        action = edge.pack_control_item(
            "SYSTEM_ACTION",
            flags=int(edge.ItemFlag.REQUIRED),
            action_kind=int(edge.SystemActionKind.HARD_DISABLE_TX),
            action_flags=0,
            reason_code=args.reason_code,
            authorization_token=0,
        )
        return make_request(
            edge.Command.HARD_DISABLE_TX,
            transaction,
            args.revision,
            items=(action,),
        )
    if args.command == "set-rf":
        rf = edge.pack_control_item(
            "RF_CONFIG",
            flags=int(edge.ItemFlag.REQUIRED),
            center_frequency_hz=args.frequency,
            internal_sample_rate_hz=edge.INTERNAL_SAMPLE_RATE_HZ,
            egress_sample_rate_hz=edge.EGRESS_SAMPLE_RATE_HZ,
            rf_bandwidth_hz=args.bandwidth,
            rx1_gain_mdb=args.rx1_gain_mdb,
            rx2_gain_mdb=args.rx2_gain_mdb,
            channel_mask=args.channels,
            rx1_gain_mode=int(GAIN_MODES[args.rx1_gain_mode]),
            rx2_gain_mode=int(GAIN_MODES[args.rx2_gain_mode]),
            expected_configuration_revision=args.revision,
            rf_flags=0,  # TX is intentionally not exposed by this CLI.
        )
        changed = (
            int(edge.ChangedField.CENTER_FREQUENCY)
            | int(edge.ChangedField.RF_BANDWIDTH)
            | int(edge.ChangedField.ANALOG_GAIN)
        )
        commit = atomic_item(
            args.revision,
            args.calibration_revision,
            args.activate_at,
            changed,
        )
        return make_request(
            edge.Command.SET_RF,
            transaction,
            args.revision,
            items=(rf, commit),
            atomic=True,
            activation_timestamp=args.activate_at,
        )
    if args.command == "configure-pipeline":
        commit = atomic_item(
            args.revision,
            args.calibration_revision,
            args.activate_at,
            args.changed_fields,
        )
        pipeline = edge.pack_control_item(
            "PIPELINE_CONFIG",
            flags=int(edge.ItemFlag.REQUIRED),
            enable_mask=(
                int(edge.PipelineBlock.RAW_TAP)
                | int(edge.PipelineBlock.NORMALIZER)
                | int(edge.PipelineBlock.QUANTIZER)
            ),
            bypass_mask=0,
            output_product_mask=PRODUCT_BITS["normalized"],
            sample_format=int(edge.SampleFormat.S8),
            normalization_strategy=int(edge.NormalizationStrategy.FIXED_SHIFT),
            headroom_bits=1,
            rounding_mode=int(edge.RoundingMode.ROUND_TO_NEAREST_EVEN),
            fixed_shift=4,
            reserved8=0,
            reserved16=0,
            expected_configuration_revision=args.revision,
            calibration_revision=args.calibration_revision,
            pipeline_revision=args.revision + 1,
            validity_policy=0,
            pipeline_flags=0,
        )
        return make_request(
            edge.Command.CONFIGURE_PIPELINE,
            transaction,
            args.revision,
            items=(commit, pipeline),
            atomic=True,
            activation_timestamp=args.activate_at,
        )
    command = {
        "create-stream": edge.Command.CREATE_STREAM,
        "start-stream": edge.Command.START_STREAM,
        "stop-stream": edge.Command.STOP_STREAM,
        "destroy-stream": edge.Command.DESTROY_STREAM,
    }[args.command]
    address = ipaddress.ip_address(args.destination)
    if address.version != 4 or address.is_multicast or address.is_unspecified:
        raise ValueError("destination must be a specific unicast IPv4 address")
    stream = edge.pack_control_item(
        "STREAM_CONFIG",
        flags=int(edge.ItemFlag.REQUIRED),
        stream_id=args.stream_id,
        destination_ipv4=address.packed,
        destination_port=args.port,
        mtu=args.mtu,
        product_mask=PRODUCT_BITS[args.product],
        sample_format=int(FORMATS[args.format]),
        channel_mask=args.channels,
        reserved16=0,
        packet_sample_count=args.samples_per_packet,
        stream_flags=0,
        reserved32=0,
    )
    atomic = command in (edge.Command.START_STREAM, edge.Command.STOP_STREAM)
    items = (stream,)
    if atomic:
        commit = atomic_item(
            args.revision,
            args.calibration_revision,
            args.activate_at,
            int(edge.ChangedField.STREAM_DESTINATION),
        )
        items = (commit, stream)
    return make_request(
        command,
        transaction,
        args.revision,
        items=items,
        atomic=atomic,
        activation_timestamp=args.activate_at,
    )


def json_default(value: object) -> object:
    if isinstance(value, bytes):
        return value.hex()
    raise TypeError(type(value).__name__)


def main() -> int:
    args = parser().parse_args()
    transaction = args.transaction_id or (secrets.randbits(31) + 1)
    try:
        request = build(args, transaction)
        transport = (
            open_unix_control(args.unix)
            if args.unix
            else open_raw_control(args.raw_device)
        )
        try:
            response, events = transport.transact(request)
        finally:
            transport.close()
    except (OSError, EOFError, ValueError, edge.ProtocolError) as error:
        print("control error: %s" % error, file=sys.stderr)
        return 1
    output = {"response": response, "events_before_response": events}
    print(json.dumps(output, indent=2, sort_keys=True, default=json_default))
    status = int(response["header"]["status"])
    return 0 if status == int(edge.Status.OK) else 3


if __name__ == "__main__":
    raise SystemExit(main())
