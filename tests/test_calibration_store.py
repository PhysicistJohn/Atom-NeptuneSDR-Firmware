"""Persistent calibration integrity, chaining, and rollback tests."""

from contextlib import redirect_stdout
import copy
import io
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest import mock

from neptunesdr_firmwave.calibration import (
    BUNDLE_SCHEMA,
    CalibrationError,
    CalibrationStore,
    bundle_sha256,
    validate_bundle,
)
from neptunesdr_firmwave.cli import main as cli_main


def bundle(revision=0, previous=None):
    value = {
        "schema": BUNDLE_SCHEMA,
        "device_serial": "NEPTUNE-TEST-001",
        "revision": revision,
        "previous_revision": previous,
        "tables": [{
            "calibration_type": "IQ_IMBALANCE",
            "channel_mask": 1,
            "frequency_min_hz": 70_000_000,
            "frequency_max_hz": 6_000_000_000,
            "sample_rate_hz": 61_440_000,
            "bandwidth_hz": 50_000_000,
            "gain_min_mdb": -10_000,
            "gain_max_mdb": 70_000,
            "temperature_min_mc": -20_000,
            "temperature_max_mc": 90_000,
            "coefficients_q2_30": [1 << 30, 0, 0, 0],
        }],
    }
    value["integrity_sha256"] = bundle_sha256(value)
    return value


class CalibrationStoreTests(unittest.TestCase):
    def test_full_table_key_allows_distinct_ranges_and_rejects_duplicates(self):
        base = bundle()["tables"][0]
        distinct_values = {
            "channel_mask": 2,
            "frequency_min_hz": 80_000_000,
            "frequency_max_hz": 5_000_000_000,
            "sample_rate_hz": 55_000_000,
            "bandwidth_hz": 40_000_000,
            "gain_min_mdb": -9_000,
            "gain_max_mdb": 69_000,
            "temperature_min_mc": -19_000,
            "temperature_max_mc": 89_000,
            "calibration_type": "DC_OFFSET",
        }
        for field, distinct in distinct_values.items():
            with self.subTest(field=field):
                value = bundle()
                second = copy.deepcopy(base)
                second[field] = distinct
                value["tables"] = [copy.deepcopy(base), second]
                value["integrity_sha256"] = bundle_sha256(value)
                validated = validate_bundle(value)
                self.assertEqual(len(validated["tables"]), 2)

        duplicate = bundle()
        duplicate["tables"].append(copy.deepcopy(duplicate["tables"][0]))
        duplicate["integrity_sha256"] = bundle_sha256(duplicate)
        with self.assertRaisesRegex(CalibrationError, "duplicate calibration table key"):
            validate_bundle(duplicate)

        same_key_different_payload = bundle()
        second = copy.deepcopy(same_key_different_payload["tables"][0])
        second["coefficients_q2_30"][0] -= 1
        same_key_different_payload["tables"].append(second)
        same_key_different_payload["integrity_sha256"] = bundle_sha256(
            same_key_different_payload
        )
        with self.assertRaisesRegex(CalibrationError, "duplicate calibration table key"):
            validate_bundle(same_key_different_payload)

    def test_integrity_and_device_identity_are_fail_closed(self):
        value = bundle()
        validate_bundle(value, expected_serial="NEPTUNE-TEST-001")
        value["tables"][0]["coefficients_q2_30"][0] -= 1
        with self.assertRaises(CalibrationError):
            validate_bundle(value)

    def test_monotonic_install_and_rollback_preserve_revisions(self):
        with tempfile.TemporaryDirectory(prefix="neptune-calibration-") as temporary:
            store = CalibrationStore(Path(temporary), "NEPTUNE-TEST-001")
            first = store.install(bundle(), activation_timestamp=100)
            self.assertEqual(first["active_revision"], 0)
            second = store.install(bundle(1, 0), activation_timestamp=200)
            self.assertEqual(second["active_revision"], 1)
            rolled_back = store.rollback(0, activation_timestamp=300)
            self.assertEqual(rolled_back["active_revision"], 0)
            self.assertTrue((Path(temporary) / "revision-0000000001.json").is_file())

    def test_revision_chain_and_first_revision_are_enforced(self):
        with tempfile.TemporaryDirectory(prefix="neptune-calibration-") as temporary:
            store = CalibrationStore(Path(temporary), "NEPTUNE-TEST-001")
            with self.assertRaises(CalibrationError):
                store.install(bundle(1, 0), activation_timestamp=1)
            store.install(bundle(), activation_timestamp=2)
            with self.assertRaises(CalibrationError):
                store.install(bundle(2, 1), activation_timestamp=3)

    def test_crash_orphan_is_reconciled_only_for_identical_content(self):
        with tempfile.TemporaryDirectory(prefix="neptune-calibration-crash-") as temporary:
            store = CalibrationStore(Path(temporary), "NEPTUNE-TEST-001")
            original = store._atomic_write
            calls = []

            def fail_index(path, value):
                calls.append(path)
                if path == store.index_path:
                    raise OSError("simulated power loss before index replace")
                return original(path, value)

            with mock.patch.object(store, "_atomic_write", side_effect=fail_index):
                with self.assertRaisesRegex(OSError, "power loss"):
                    store.install(bundle(), activation_timestamp=10)
            self.assertIsNone(store.active_index())
            self.assertTrue(store._bundle_path(0).is_file())
            recovered = store.install(bundle(), activation_timestamp=11)
            self.assertEqual(recovered["active_revision"], 0)
            self.assertEqual(recovered["activation_timestamp"], 11)

    def test_concurrent_revision_install_is_serialized(self):
        with tempfile.TemporaryDirectory(prefix="neptune-calibration-lock-") as temporary:
            root = Path(temporary)
            CalibrationStore(root, "NEPTUNE-TEST-001").install(
                bundle(), activation_timestamp=1
            )
            barrier = threading.Barrier(2)
            results = []

            def install_revision():
                store = CalibrationStore(root, "NEPTUNE-TEST-001")
                barrier.wait()
                try:
                    store.install(bundle(1, 0), activation_timestamp=2)
                except CalibrationError as exc:
                    results.append(type(exc).__name__)
                else:
                    results.append("installed")

            threads = [threading.Thread(target=install_revision) for _ in range(2)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=5)
                self.assertFalse(thread.is_alive())
            self.assertEqual(sorted(results), ["CalibrationError", "installed"])
            self.assertEqual(
                CalibrationStore(root, "NEPTUNE-TEST-001").active_index()["active_revision"],
                1,
            )

    def test_validation_cli_reports_identity_without_mutating_state(self):
        with tempfile.TemporaryDirectory(prefix="neptune-calibration-cli-") as temporary:
            path = Path(temporary) / "bundle.json"
            path.write_text(json.dumps(bundle()), encoding="utf-8")
            output = io.StringIO()
            with redirect_stdout(output):
                status = cli_main([
                    "validate-calibration",
                    str(path),
                    "--device-serial",
                    "NEPTUNE-TEST-001",
                ])
            self.assertEqual(status, 0)
            self.assertIn("revision=0", output.getvalue())
            self.assertEqual(tuple(Path(temporary).iterdir()), (path,))


if __name__ == "__main__":
    unittest.main()
