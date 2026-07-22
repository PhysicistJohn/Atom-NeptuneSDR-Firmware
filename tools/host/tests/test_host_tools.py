import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest


HOST = Path(__file__).resolve().parents[1]
ROOT = HOST.parents[1]
sys.path.insert(0, str(HOST))

from neptune_edge_host import (  # noqa: E402
    PacketTracker,
    edge,
    make_request,
)
import neptune_ctl  # noqa: E402


class HostProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.vectors = json.loads(
            (ROOT / "protocol" / "golden" / "neptune_edge_v1_vectors.json").read_text()
        )["vectors"]

    def test_request_uses_generated_control_binding(self):
        message = make_request(edge.Command.GET_IDENTITY, 17, 0)
        decoded = edge.unpack_control_message(message)
        self.assertEqual(decoded["header"]["transaction_id"], 17)
        self.assertEqual(decoded["header"]["command_id"], edge.Command.GET_IDENTITY)

    def test_tracker_validates_golden_packet_and_reports_repetition(self):
        vector = next(
            value
            for value in self.vectors
            if value["name"] == "normalized_s8bf_dual_rx_55msps"
        )
        packet = bytes.fromhex(vector["wire_hex"])
        tracker = PacketTracker()
        first = tracker.accept(packet)
        self.assertFalse(first["sequence_gap"])
        self.assertFalse(first["timing_break"])
        second = tracker.accept(packet)
        self.assertTrue(second["sequence_gap"])
        self.assertTrue(second["timing_break"])

    def test_tracker_rejects_corrupt_payload(self):
        vector = next(value for value in self.vectors if value["kind"] == "data")
        packet = bytearray.fromhex(vector["wire_hex"])
        packet[-1] ^= 1
        with self.assertRaises(edge.ProtocolError):
            PacketTracker().accept(packet)

    def test_cli_builds_canonical_pipeline_counters_and_hard_disable(self):
        pipeline_args = neptune_ctl.parser().parse_args(
            [
                "--unix",
                "/tmp/control.sock",
                "configure-pipeline",
                "--revision",
                "7",
                "--calibration-revision",
                "3",
                "--changed-fields",
                str(int(edge.ChangedField.QUANTIZATION)),
            ]
        )
        pipeline = edge.unpack_control_message(neptune_ctl.build(pipeline_args, 91))
        self.assertEqual(
            [item["name"] for item in pipeline["items"]],
            ["ATOMIC_COMMIT", "PIPELINE_CONFIG"],
        )
        config = pipeline["items"][1]["values"]
        self.assertEqual(config["output_product_mask"], 1 << int(edge.PacketType.NORMALIZED_IQ))
        self.assertEqual(config["sample_format"], int(edge.SampleFormat.S8))
        self.assertEqual(
            config["rounding_mode"], int(edge.RoundingMode.ROUND_TO_NEAREST_EVEN)
        )
        self.assertEqual(config["expected_configuration_revision"], 7)
        self.assertEqual(config["calibration_revision"], 3)

        counters_args = neptune_ctl.parser().parse_args(
            ["--unix", "/tmp/control.sock", "get-counters"]
        )
        counters = edge.unpack_control_message(neptune_ctl.build(counters_args, 92))
        self.assertEqual(counters["header"]["command_id"], int(edge.Command.GET_COUNTERS))
        self.assertEqual(counters["items"], ())

        disable_args = neptune_ctl.parser().parse_args(
            [
                "--unix",
                "/tmp/control.sock",
                "hard-disable-tx",
                "--revision",
                "7",
                "--reason-code",
                "0x54584f46",
            ]
        )
        disable = edge.unpack_control_message(neptune_ctl.build(disable_args, 93))
        self.assertEqual(
            disable["header"]["command_id"], int(edge.Command.HARD_DISABLE_TX)
        )
        action = disable["items"][0]["values"]
        self.assertEqual(action["action_kind"], int(edge.SystemActionKind.HARD_DISABLE_TX))
        self.assertEqual(action["authorization_token"], 0)


class HostDaemonIntegrationTests(unittest.TestCase):
    def test_identity_over_unix_transport(self):
        daemon = ROOT / "ps" / "control-daemon" / "build" / "neptune-control-daemon"
        self.assertTrue(daemon.is_file(), "control daemon must be built before host tests")
        with tempfile.TemporaryDirectory() as temporary:
            socket_path = Path(temporary) / "control.sock"
            process = subprocess.Popen(
                [str(daemon), "--mock", "--unix", str(socket_path)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                deadline = time.monotonic() + 3
                while not socket_path.exists() and time.monotonic() < deadline:
                    if process.poll() is not None:
                        self.fail("daemon exited early: %s" % process.stderr.read())
                    time.sleep(0.01)
                result = subprocess.run(
                    [
                        sys.executable,
                        str(HOST / "neptune_ctl.py"),
                        "--unix",
                        str(socket_path),
                        "identity",
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=5,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                output = json.loads(result.stdout)
                items = output["response"]["items"]
                identity = next(item for item in items if item["name"] == "IDENTITY")
                safety = next(item for item in items if item["name"] == "SAFETY")
                self.assertEqual(identity["hardware_id"], 0x50323130)
                self.assertEqual(identity["firmware_build_id"], 0x4D4F434B00000001)
                self.assertEqual(identity["protocol_data_sha256"], edge.DATA_SPEC_SHA256)
                self.assertEqual(
                    identity["protocol_control_sha256"], edge.CONTROL_SPEC_SHA256
                )
                self.assertFalse(safety["tx_enabled"])
                self.assertTrue(safety["tx_inhibited"])
                health_result = subprocess.run(
                    [
                        sys.executable,
                        str(HOST / "neptune_ctl.py"),
                        "--unix",
                        str(socket_path),
                        "health",
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=5,
                    check=False,
                )
                self.assertEqual(health_result.returncode, 0, health_result.stderr)
                health_output = json.loads(health_result.stdout)
                health_items = health_output["response"]["items"]
                self.assertEqual(
                    [item["name"] for item in health_items], ["HEALTH", "SAFETY"]
                )
                health = health_items[0]
                self.assertEqual(health["uptime_ns"], 1_000_000_000)
                self.assertEqual(health["pll_lock_mask"], 3)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
                if process.stderr is not None:
                    process.stderr.close()

    def test_one_shot_atomic_command_receives_its_state_event(self):
        daemon = ROOT / "ps" / "control-daemon" / "build" / "neptune-control-daemon"
        self.assertTrue(daemon.is_file(), "control daemon must be built before host tests")
        with tempfile.TemporaryDirectory() as temporary:
            socket_path = Path(temporary) / "control.sock"
            process = subprocess.Popen(
                [str(daemon), "--mock", "--unix", str(socket_path)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                deadline = time.monotonic() + 3
                while not socket_path.exists() and time.monotonic() < deadline:
                    if process.poll() is not None:
                        self.fail("daemon exited early: %s" % process.stderr.read())
                    time.sleep(0.01)
                result = subprocess.run(
                    [
                        sys.executable,
                        str(HOST / "neptune_ctl.py"),
                        "--unix",
                        str(socket_path),
                        "set-rf",
                        "--revision",
                        "0",
                        "--frequency",
                        "915000000",
                        "--bandwidth",
                        "50000000",
                        "--rx1-gain-mdb",
                        "20000",
                        "--rx2-gain-mdb",
                        "20000",
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=5,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                output = json.loads(result.stdout)
                self.assertEqual(len(output["events_before_response"]), 1)
                event = output["events_before_response"][0]
                self.assertEqual(event["header"]["message_kind"], int(edge.MessageKind.EVENT))
                self.assertEqual(event["header"]["command_id"], int(edge.Command.STATE_CHANGE_EVENT))
                self.assertEqual(
                    event["header"]["activation_timestamp"],
                    output["response"]["header"]["activation_timestamp"],
                )

                def stream_command(name, revision):
                    return subprocess.run(
                        [
                            sys.executable,
                            str(HOST / "neptune_ctl.py"),
                            "--unix",
                            str(socket_path),
                            name,
                            "--revision",
                            str(revision),
                            "--stream-id",
                            "9",
                            "--destination",
                            "192.0.2.1",
                            "--port",
                            "50000",
                            "--mtu",
                            "9000",
                            "--samples-per-packet",
                            "4096",
                        ],
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        timeout=5,
                        check=False,
                    )

                create = stream_command("create-stream", 1)
                self.assertEqual(create.returncode, 0, create.stderr)
                create_output = json.loads(create.stdout)
                self.assertEqual(create_output["events_before_response"], [])
                self.assertEqual(
                    [item["name"] for item in create_output["response"]["items"]],
                    ["STREAM_STATUS"],
                )
                self.assertEqual(
                    create_output["response"]["items"][0]["stream_state"],
                    int(edge.StreamState.CREATED),
                )

                start = stream_command("start-stream", 1)
                self.assertEqual(start.returncode, 0, start.stderr)
                start_output = json.loads(start.stdout)
                self.assertEqual(len(start_output["events_before_response"]), 1)
                self.assertEqual(
                    [item["name"] for item in start_output["response"]["items"]],
                    ["STATE_CHANGE", "STREAM_STATUS"],
                )
                self.assertEqual(
                    start_output["response"]["items"][1]["stream_state"],
                    int(edge.StreamState.RUNNING),
                )

                stop = stream_command("stop-stream", 2)
                self.assertEqual(stop.returncode, 0, stop.stderr)
                stop_output = json.loads(stop.stdout)
                self.assertEqual(len(stop_output["events_before_response"]), 1)
                self.assertEqual(
                    stop_output["response"]["items"][1]["stream_state"],
                    int(edge.StreamState.CREATED),
                )

                destroy = stream_command("destroy-stream", 3)
                self.assertEqual(destroy.returncode, 0, destroy.stderr)
                destroy_output = json.loads(destroy.stdout)
                self.assertEqual(destroy_output["events_before_response"], [])
                self.assertEqual(
                    destroy_output["response"]["items"][0]["stream_state"],
                    int(edge.StreamState.NONE),
                )
            finally:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
                if process.stderr is not None:
                    process.stderr.close()


if __name__ == "__main__":
    unittest.main()
