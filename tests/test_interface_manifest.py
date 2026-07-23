"""Canonical interface and relocatable runtime-manifest proofs."""

import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from neptunesdr_firmware.interface import INTERFACE_SCHEMA, interface_path, interface_sha256, load_interface
from neptunesdr_firmware.manifest import RUNTIME_MANIFEST_SCHEMA, finish_runtime_manifest


ROOT = Path(__file__).resolve().parents[1]


def _committed_repository(path: Path) -> None:
    subprocess.run(("git", "init", "-b", "main"), cwd=path, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(("git", "config", "user.name", "Firmware Test"), cwd=path, check=True)
    subprocess.run(("git", "config", "user.email", "firmware@example.invalid"), cwd=path, check=True)
    (path / "source.txt").write_text("fixed source\n", encoding="utf-8")
    subprocess.run(("git", "add", "source.txt"), cwd=path, check=True)
    subprocess.run(("git", "commit", "-m", "fixture"), cwd=path, check=True, stdout=subprocess.DEVNULL)


class InterfaceTests(unittest.TestCase):
    def test_canonical_interface_carries_wideband_fft_and_transport_contacts(self):
        interface = load_interface()
        self.assertEqual(interface["schema"], INTERFACE_SCHEMA)
        self.assertEqual(interface["repository"]["name"], "Atom-NeptuneSDR-Firmware")
        self.assertEqual(interface["profile"], "qemu-development")
        self.assertIs(interface["flashable"], False)
        self.assertEqual(interface["wideband_capture"]["sample_rate_hz"], 61_440_000)
        self.assertEqual(interface["wideband_capture"]["rf_bandwidth_hz"], 50_000_000)
        self.assertEqual(interface["pl_fft_abi"]["maximum_log2_n"], 16)
        self.assertEqual(interface["spectrum_stream"]["protocol"], "NSFT-v1")

    def test_interface_sha_is_the_exact_canonical_file(self):
        expected = hashlib.sha256(interface_path().read_bytes()).hexdigest()
        self.assertEqual(interface_sha256(), expected)
        self.assertRegex(expected, r"^[0-9a-f]{64}$")

    def test_guest_constants_refine_the_canonical_interface(self):
        interface = load_interface()
        abi = interface["pl_fft_abi"]
        stream = interface["spectrum_stream"]
        wideband = interface["wideband_capture"]
        source = (ROOT / "firmware" / "neptune_fft_streamer.c").read_text(encoding="utf-8")

        def body(name):
            match = re.search(r"^#define\s+%s\s+([^\n]+)" % re.escape(name), source, re.MULTILINE)
            self.assertIsNotNone(match, name)
            return match.group(1).strip()

        def integer(name):
            raw = body(name)
            for pattern, base in (
                (r"UINT32_C\((0x[0-9a-fA-F]+|\d+)\)", 0),
                (r"(0x[0-9a-fA-F]+|\d+)U?", 0),
            ):
                match = re.fullmatch(pattern, raw)
                if match:
                    return int(match.group(1), base)
            shift = re.fullmatch(r"\((?:UINT32_C\()?1\)?U?\s*<<\s*(\d+)\)", raw)
            self.assertIsNotNone(shift, "%s=%s" % (name, raw))
            return 1 << int(shift.group(1))

        registers = {
            name.removeprefix("FFT_REG_"): value
            for name, value in (
                (match.group(1), int(match.group(2), 16))
                for match in re.finditer(
                    r"^#define\s+(FFT_REG_[A-Z0-9_]+)\s+0x([0-9a-fA-F]+)U",
                    source,
                    re.MULTILINE,
                )
            )
        }
        self.assertEqual(
            registers,
            {name: int(abi["registers"][name], 16) for name in registers},
        )
        direct = {
            "FFT_PHYS_BASE": int(abi["base_address"], 16),
            "FFT_MMIO_BYTES": int(abi["span_bytes"], 16),
            "FFT_INPUT_PHYS": int(abi["input_address"], 16),
            "FFT_OUTPUT_PHYS": int(abi["output_address"], 16),
            "FFT_LOG2_N": abi["firmware_log2_n"],
            "FFT_CHANNELS": abi["firmware_channels"],
            "FFT_CHANNEL_MASK": int(abi["firmware_channel_mask"], 16),
            "FFT_ID": int(abi["identity"], 16),
            "FFT_ABI_MAJOR": int(abi["version"], 16),
            "FFT_CONTROL_START": int(abi["control_bits"]["START"], 16),
            "FFT_STATUS_BUSY": int(abi["status_bits"]["BUSY"], 16),
            "FFT_STATUS_DONE": int(abi["status_bits"]["DONE"], 16),
            "FFT_STATUS_ERROR": int(abi["status_bits"]["ERROR"], 16),
            "STREAM_PORT": interface["host_services"]["spectrum"]["port"],
            "NSFT_HEADER_BYTES": stream["header_bytes"],
            "NSFT_CRC_BYTES": stream["trailer_crc_bytes"],
            "NSFT_ENCODING_UINT16_LOG": stream["payload_encoding_code"],
            "NSFT_PACKET_VERSION": stream["packet_version"],
        }
        for name, value in direct.items():
            self.assertEqual(integer(name), value, name)

        capability_names = {
            "FFT_CAP_IQ16_LE": "IQ16_LE",
            "FFT_CAP_POWER_U32_LE": "POWER_U32_LE",
            "FFT_CAP_TWO_CHANNEL": "TWO_CHANNEL",
            "FFT_CAP_SCALE_EACH_STAGE": "SCALE_EACH_STAGE",
            "FFT_CAP_NATURAL_ORDER": "NATURAL_ORDER",
        }
        for macro, key in capability_names.items():
            self.assertEqual(integer(macro), int(abi["capability_bits"][key], 16), macro)
        self.assertEqual(
            sum(integer(name) for name in capability_names),
            int(abi["guest_required_capabilities_value"], 16),
        )

        n = 1 << integer("FFT_LOG2_N")
        self.assertEqual(n, stream["fft_bins_per_channel"])
        self.assertEqual(n * integer("FFT_CHANNELS") * 4, abi["firmware_input_bytes"])
        selected_channels = bin(integer("FFT_CHANNEL_MASK")).count("1")
        self.assertEqual(n * selected_channels * 4, abi["firmware_output_bytes"])
        self.assertEqual(
            integer("NSFT_HEADER_BYTES") + n * stream["payload_bytes_per_bin"] + integer("NSFT_CRC_BYTES"),
            stream["packet_bytes_per_channel"],
        )
        self.assertEqual(body("FFT_N"), "(UINT32_C(1) << FFT_LOG2_N)")
        self.assertEqual(body("FFT_INPUT_BYTES"), "(FFT_N * FFT_CHANNELS * 4U)")
        self.assertEqual(body("FFT_OUTPUT_BYTES"), "(FFT_N * FFT_CHANNELS * 4U)")
        self.assertEqual(body("NSFT_PACKET_BYTES"), "(NSFT_HEADER_BYTES + FFT_N * 2U + NSFT_CRC_BYTES)")
        self.assertEqual(body("RF_BANDWIDTH_HZ"), '"%d\\n"' % wideband["rf_bandwidth_hz"])
        self.assertEqual(body("SAMPLE_RATE_TEXT"), '"%d\\n"' % wideband["sample_rate_hz"])
        self.assertEqual(float(body("NSFT_DB_FLOOR").strip("()")), stream["payload_db_floor"])
        self.assertEqual(float(body("NSFT_DB_STEP")), stream["payload_db_step"])
        self.assertEqual(float(body("AD9361_ADC_FULL_SCALE")), wideband["adc_full_scale_code"])

        all_contract_macros = {
            match.group(1)
            for match in re.finditer(
                r"^#define\s+((?:FFT|RF_BANDWIDTH_HZ|SAMPLE_RATE_TEXT|STREAM_PORT|NSFT|AD9361_ADC_FULL_SCALE)[A-Z0-9_]*)\s+",
                source,
                re.MULTILINE,
            )
        }
        covered = set(direct) | set(capability_names) | {
            "FFT_N", "FFT_INPUT_BYTES", "FFT_OUTPUT_BYTES", "FFT_CAPABILITIES_REQUIRED",
            "RF_BANDWIDTH_HZ", "SAMPLE_RATE_TEXT", "NSFT_DB_FLOOR", "NSFT_DB_STEP",
            "NSFT_PACKET_BYTES", "AD9361_ADC_FULL_SCALE",
        } | {"FFT_REG_" + name for name in registers}
        self.assertEqual(all_contract_macros, covered)


class RuntimeManifestTests(unittest.TestCase):
    def test_manifest_paths_are_relative_and_every_output_is_hashed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            output = root / "runtime"
            source.mkdir()
            output.mkdir()
            _committed_repository(source)
            kernel = output / "p210-kernel.bin"
            rootfs = output / "nested" / "rootfs.cpio.gz"
            kernel.write_bytes(b"kernel")
            rootfs.parent.mkdir()
            rootfs.write_bytes(b"rootfs")
            manifest = finish_runtime_manifest(
                {},
                (
                    ("kernel", kernel, "kernel"),
                    ("rootfs", rootfs, "rootfs"),
                ),
                output_root=output,
                source_root=source,
            )
        self.assertEqual(manifest["schema"], RUNTIME_MANIFEST_SCHEMA)
        self.assertEqual(manifest["profile"], "qemu-development")
        self.assertIs(manifest["flashable"], False)
        self.assertEqual(manifest["interface"]["path"], "specs/p210-firmware-interface-v1.json")
        self.assertFalse(Path(manifest["interface"]["path"]).is_absolute())
        self.assertEqual(manifest["generated_artifacts"]["kernel"]["path"], "p210-kernel.bin")
        self.assertEqual(manifest["generated_artifacts"]["rootfs"]["path"], "nested/rootfs.cpio.gz")
        self.assertEqual(manifest["generated_artifacts"]["kernel"]["bytes"], 6)
        self.assertEqual(
            manifest["generated_artifacts"]["kernel"]["sha256"],
            hashlib.sha256(b"kernel").hexdigest(),
        )
        self.assertEqual(manifest["firmware_source"]["repository"], "Atom-NeptuneSDR-Firmware")
        self.assertTrue(manifest["firmware_source"]["clean"])
        self.assertTrue(manifest["artifact_hashes_complete"])

    def test_manifest_rejects_an_artifact_outside_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            output = root / "runtime"
            source.mkdir()
            output.mkdir()
            _committed_repository(source)
            outside = root / "outside.bin"
            outside.write_bytes(b"outside")
            with self.assertRaisesRegex(ValueError, "escapes runtime output"):
                finish_runtime_manifest(
                    {},
                    (("outside", outside, "invalid"),),
                    output_root=output,
                    source_root=source,
                )


if __name__ == "__main__":
    unittest.main()
