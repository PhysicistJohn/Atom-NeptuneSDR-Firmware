"""P210 hardware-evidence manifests fail closed across artifact generations."""

import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import unittest

from neptunesdr_firmwave.cli import main
from neptunesdr_firmwave.errors import HardwareManifestError
from neptunesdr_firmwave.hardware_manifest import (
    HARDWARE_MANIFEST_SCHEMA,
    hardware_schema_path,
    load_hardware_manifest,
    validate_hardware_manifest,
)


def _write_manifest(root, value, name="hardware.json"):
    path = Path(root) / name
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


def _normalized_candidate():
    value = copy.deepcopy(load_hardware_manifest())
    identity = {
        "repository": "https://github.com/example/hardware",
        "revision": "matched-build-1",
        "path": "projects/p210",
    }
    for artifact in value["artifacts"]:
        contacts = artifact["contacts"]
        contacts["part"] = "xc7z020clg400-1"
        contacts["ddr"] = {"bus_width_bits": 16, "size_bytes": 536870912}
        contacts["radio"] = {
            "digital_interface": "cmos",
            "rx_channels": 2,
            "tx_channels": 2,
        }
        contacts["gpio"]["mio_count"] = 54
        contacts["gpio"]["emio_width"] = 64
        contacts["build"] = {
            "vivado_version": "2023.2",
            "source_identity": copy.deepcopy(identity),
        }
    return value


class HardwareManifestTests(unittest.TestCase):
    def test_checked_in_schema_and_candidate_are_versioned(self):
        schema = json.loads(hardware_schema_path().read_text(encoding="utf-8"))
        manifest = load_hardware_manifest()
        self.assertEqual(manifest["schema"], HARDWARE_MANIFEST_SCHEMA)
        self.assertEqual(schema["properties"]["schema"]["const"], HARDWARE_MANIFEST_SCHEMA)
        self.assertIs(manifest["flashable"], False)

    def test_locked_candidate_exposes_every_known_cross_artifact_mismatch(self):
        report = validate_hardware_manifest()
        self.assertFalse(report.compatible)
        checks = {issue.check for issue in report.issues}
        expected = {
            "consistency.ddr.bus_width_bits",
            "consistency.ddr.size_bytes",
            "consistency.radio.digital_interface",
            "consistency.radio.rx_channels",
            "consistency.radio.tx_channels",
            "consistency.gpio.emio_width",
            "consistency.build.vivado_version",
            "consistency.build.source_identity",
            "gpio.capacity.system-xsa",
        }
        self.assertEqual(checks, expected)
        self.assertEqual(report.consensus["part"], "xc7z020clg400-1")
        self.assertEqual(report.consensus["gpio.mio_count"], 54)

        width = next(
            issue for issue in report.issues if issue.check == "consistency.ddr.bus_width_bits"
        )
        self.assertEqual(width.observed, {"sd-boot-hdf": 32, "system-xsa": 16})
        interface = next(
            issue for issue in report.issues if issue.check == "consistency.radio.digital_interface"
        )
        self.assertEqual(
            interface.observed,
            {"sd-boot-hdf": "lvds", "system-xsa": "cmos", "public-devicetree": "lvds"},
        )

    def test_consistent_candidate_passes_without_weakening_requirements(self):
        with tempfile.TemporaryDirectory() as directory:
            path = _write_manifest(directory, _normalized_candidate())
            report = validate_hardware_manifest(path)
        self.assertTrue(report.compatible, report.to_dict())
        self.assertEqual(report.issues, [])
        self.assertEqual(report.consensus["radio.rx_channels"], 2)

    def test_missing_required_observations_fail_coverage(self):
        value = _normalized_candidate()
        for artifact in value["artifacts"]:
            artifact["contacts"]["build"]["source_identity"] = None
        with tempfile.TemporaryDirectory() as directory:
            report = validate_hardware_manifest(_write_manifest(directory, value))
        self.assertFalse(report.compatible)
        self.assertIn("coverage.build.source_identity", {issue.check for issue in report.issues})

    def test_gpio_mapping_is_checked_against_each_handoff_capacity(self):
        value = _normalized_candidate()
        xsa = next(item for item in value["artifacts"] if item["id"] == "system-xsa")
        xsa["contacts"]["gpio"]["emio_width"] = 18
        with tempfile.TemporaryDirectory() as directory:
            report = validate_hardware_manifest(_write_manifest(directory, value))
        checks = {issue.check for issue in report.issues}
        self.assertIn("consistency.gpio.emio_width", checks)
        self.assertIn("gpio.capacity.system-xsa", checks)

    def test_part_and_control_mapping_disagreement_fail_closed(self):
        value = _normalized_candidate()
        xsa = next(item for item in value["artifacts"] if item["id"] == "system-xsa")
        xsa["contacts"]["part"] = "xc7z010clg400-1"
        xsa["contacts"]["gpio"]["control_offsets"]["usb0_reset"] = 84
        with tempfile.TemporaryDirectory() as directory:
            report = validate_hardware_manifest(_write_manifest(directory, value))
        checks = {issue.check for issue in report.issues}
        self.assertIn("consistency.part", checks)
        self.assertIn("gpio.mapping.usb0_reset", checks)

    def test_manifest_digest_must_match_the_firmware_lock(self):
        value = _normalized_candidate()
        value["artifacts"][0]["source"]["sha256"] = "0" * 64
        with tempfile.TemporaryDirectory() as directory:
            report = validate_hardware_manifest(_write_manifest(directory, value))
        self.assertFalse(report.compatible)
        self.assertIn("lock.sd-boot-hdf", {issue.check for issue in report.issues})

    def test_duplicate_artifact_ids_are_structural_errors(self):
        value = _normalized_candidate()
        value["artifacts"][1]["id"] = value["artifacts"][0]["id"]
        with tempfile.TemporaryDirectory() as directory:
            path = _write_manifest(directory, value)
            with self.assertRaisesRegex(HardwareManifestError, "artifact IDs are not unique"):
                load_hardware_manifest(path)

    def test_gpio_offsets_reject_null_instead_of_failing_during_comparison(self):
        value = _normalized_candidate()
        value["artifacts"][0]["contacts"]["gpio"]["control_offsets"]["usb0_reset"] = None
        with tempfile.TemporaryDirectory() as directory:
            path = _write_manifest(directory, value)
            with self.assertRaisesRegex(HardwareManifestError, "is not an integer in range"):
                load_hardware_manifest(path)


class HardwareManifestCLITests(unittest.TestCase):
    def _run(self, arguments):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = main(arguments)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_default_candidate_is_machine_readable_and_returns_incompatible(self):
        code, output, error = self._run(["validate-hardware-manifest", "--json"])
        self.assertEqual((code, error), (1, ""))
        value = json.loads(output)
        self.assertIs(value["compatible"], False)
        self.assertEqual(value["candidate_id"], "community-p210-mixed-evidence")

    def test_cli_distinguishes_compatible_from_malformed(self):
        with tempfile.TemporaryDirectory() as directory:
            compatible = _write_manifest(directory, _normalized_candidate(), "compatible.json")
            code, output, error = self._run(
                ["validate-hardware-manifest", "--manifest", str(compatible), "--json"]
            )
            self.assertEqual((code, error), (0, ""))
            self.assertTrue(json.loads(output)["compatible"])

            malformed = Path(directory) / "malformed.json"
            malformed.write_text("[]", encoding="utf-8")
            code, output, error = self._run(
                ["validate-hardware-manifest", "--manifest", str(malformed), "--json"]
            )
            self.assertEqual((code, output), (2, ""))
            self.assertIn("hardware manifest must be an object", error)
            self.assertNotIn("Traceback", error)


if __name__ == "__main__":
    unittest.main()
