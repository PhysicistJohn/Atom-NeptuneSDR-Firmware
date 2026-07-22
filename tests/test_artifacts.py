"""Fail-closed firmware, rootfs, and XSA artifact boundaries."""

import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import zipfile
import zlib

from neptunesdr_firmwave.boot_harness import BootArtifacts, build_qemu_command
from neptunesdr_firmwave.errors import FirmwareFormatError
from neptunesdr_firmwave.firmware import UIMAGE_MAGIC, UImage, fetch_locked_artifact
from neptunesdr_firmwave.locks import validate_firmware_lock, validate_runtime_lock
from neptunesdr_firmwave.runtime_rootfs import CpioEntry, NewcArchive
from neptunesdr_firmwave.xsa import validate_xsa


def _entry(name, data=b"", mode=0o100644, inode=1):
    return CpioEntry(name, inode, mode, 0, 0, 1, 0, data)


def _uimage(payload):
    name = b"P210 test kernel".ljust(32, b"\0")
    header = bytearray(
        struct.pack(
            ">7I4B32s",
            UIMAGE_MAGIC,
            0,
            1,
            len(payload),
            0x8000,
            0x8000,
            zlib.crc32(payload) & 0xFFFFFFFF,
            5,
            2,
            2,
            0,
            name,
        )
    )
    struct.pack_into(">I", header, 4, zlib.crc32(header) & 0xFFFFFFFF)
    return bytes(header) + payload


def _xsa(extra_member=None):
    part = "xc7z020clg400-1"
    metadata = {
        "hardware": "true",
        "generatedVersion": "2023.2",
        "generatedTimestamp": "test",
        "devices": [{"part": {"name": part}}],
    }
    modules = (
        ("sys_ps7", "xilinx.com:ip:processing_system7:5.5"),
        ("axi_ad9361", "analog.com:user:axi_ad9361:1.0"),
        ("axi_ad9361_adc_dma", "analog.com:user:axi_dmac:1.0"),
        ("axi_ad9361_dac_dma", "analog.com:user:axi_dmac:1.0"),
        ("cpack", "analog.com:user:util_cpack2:1.0"),
        ("tx_upack", "analog.com:user:util_upack2:1.0"),
    )
    parameters = {
        "sys_ps7": {
            "PCW_ACT_APU_PERIPHERAL_FREQMHZ": "666.666687",
            "PCW_UIPARAM_DDR_BUS_WIDTH": "16 Bit",
            "PCW_UIPARAM_ACT_DDR_FREQ_MHZ": "533.333374",
            "PCW_UIPARAM_DDR_PARTNO": "MT41K256M16 RE-125",
            "PCW_CLK0_FREQ": "100000000",
        },
        "cpack": {
            "NUM_OF_CHANNELS": "4",
            "SAMPLE_DATA_WIDTH": "16",
            "SAMPLES_PER_CHANNEL": "1",
        },
        "axi_ad9361_adc_dma": {
            "DMA_DATA_WIDTH_SRC": "64",
            "DMA_DATA_WIDTH_DEST": "64",
        },
    }
    module_xml = "".join(
        '<MODULE INSTANCE="%s" VLNV="%s">%s</MODULE>'
        % (
            instance,
            vlnv,
            "".join(
                '<PARAMETER NAME="%s" VALUE="%s" />' % item
                for item in parameters.get(instance, {}).items()
            ),
        )
        for instance, vlnv in modules
    )
    ranges = (
        ("axi_ad9361", 0x79020000, 0x7902FFFF),
        ("axi_ad9361_adc_dma", 0x7C400000, 0x7C400FFF),
        ("axi_ad9361_dac_dma", 0x7C420000, 0x7C420FFF),
    )
    range_xml = "".join(
        '<MEMRANGE INSTANCE="%s" BASEVALUE="0x%08X" HIGHVALUE="0x%08X" />'
        % item
        for item in ranges
    )
    hwh = (
        '<EDKSYSTEM VIVADOVERSION="2023.2"><MODULES>%s</MODULES>'
        '<MEMORYMAP>%s</MEMORYMAP></EDKSYSTEM>' % (module_xml, range_xml)
    )
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("xsa.json", json.dumps(metadata))
        archive.writestr("sysdef.xml", '<Project><SYSTEMINFO PART="%s" /></Project>' % part)
        archive.writestr("system.hwh", hwh)
        archive.writestr("system_top.bit", b"\xaa" * 1_000_000)
        if extra_member is not None:
            archive.writestr(extra_member, b"must not be extracted")
    return output.getvalue()


class ArtifactBoundaryTests(unittest.TestCase):
    def test_rootfs_paths_symlinks_and_uimage_crc_fail_closed(self):
        archive = NewcArchive(
            (
                _entry(".", mode=0o040755),
                _entry("bin/tool", b"trusted", mode=0o100755, inode=2),
                _entry("sbin/tool", b"../bin/tool", mode=0o120777, inode=3),
            )
        )
        parsed = NewcArchive.parse(archive.to_bytes())
        self.assertEqual(parsed.read("/sbin/tool"), b"trusted")
        with self.assertRaises(FirmwareFormatError):
            NewcArchive((_entry("../escape"),))
        with self.assertRaisesRegex(FirmwareFormatError, "follows the first"):
            NewcArchive.parse(archive.to_bytes() + b"unexpected")

        image = _uimage(b"deterministic kernel payload")
        self.assertEqual(UImage(image).payload, b"deterministic kernel payload")
        corrupted = bytearray(image)
        corrupted[-1] ^= 1
        with self.assertRaisesRegex(FirmwareFormatError, "data CRC mismatch"):
            UImage(bytes(corrupted))

    def test_xsa_required_hardware_contacts_pass(self):
        report = validate_xsa(_xsa())
        self.assertTrue(report.compatible, report.issues)
        self.assertEqual(report.facts["part"], "xc7z020clg400-1")
        self.assertEqual(report.facts["hardware_contacts"]["ddr_bus_width"], "16 Bit")
        self.assertEqual(
            report.facts["address_ranges"]["axi_ad9361_adc_dma"]["base"],
            0x7C400000,
        )

    def test_xsa_rejects_archive_traversal(self):
        unsafe = validate_xsa(_xsa("../escape"))
        self.assertFalse(unsafe.compatible)
        self.assertIn("xsa.members", {issue.check for issue in unsafe.issues})

    def test_qemu_command_is_direct_kernel_and_has_no_usb_disk_or_network(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kernel = root / "kernel"
            dtb = root / "board.dtb"
            ramdisk = root / "rootfs.cpio.gz"
            for path in (kernel, dtb, ramdisk):
                path.write_bytes(path.name.encode("ascii"))
            artifacts = BootArtifacts(
                source=root / "input",
                kind="test",
                kernel=kernel,
                devicetree=dtb,
                ramdisk=ramdisk,
                bootargs="console=ttyPS0",
            )
            command = build_qemu_command(artifacts, "qemu-system-arm")
        joined = " ".join(command)
        self.assertIn("xilinx-zynq-a9", joined)
        self.assertNotIn("-drive", command)
        self.assertNotIn("-usb", command)
        self.assertNotIn("-netdev", command)
        self.assertNotIn("-device", command)


class LockBoundaryTests(unittest.TestCase):
    def test_checked_in_locks_are_consistent_and_content_addressed(self):
        firmware = validate_firmware_lock()
        runtime = validate_runtime_lock()
        self.assertEqual(
            set(firmware["artifacts"]),
            {"p210-sd-boot", "p210-system-xsa", "plutosdr-fw-v0.39"},
        )
        self.assertIn("derived runtime evidence", runtime["classification"])

    def test_lock_rejects_non_https_and_invalid_digest(self):
        lock = {
            "schema": 1,
            "artifacts": {
                "item": {"url": "http://example.test/a", "sha256": "x", "bytes": 1, "kind": "x"}
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lock.json"
            path.write_text(json.dumps(lock), encoding="utf-8")
            with self.assertRaises(FirmwareFormatError):
                validate_firmware_lock(path)

    def test_locked_fetch_is_atomic_and_rejects_wrong_bytes(self):
        payload = b"locked bytes"
        lock = {
            "schema": 1,
            "artifacts": {
                "item": {
                    "url": "https://invalid.example/item",
                    "sha256": hashlib.sha256(payload).hexdigest(),
                    "bytes": len(payload),
                    "kind": "test",
                }
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lock_path = root / "lock.json"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            destination = root / "item.bin"
            with mock.patch(
                "neptunesdr_firmwave.firmware.urllib.request.urlopen",
                return_value=io.BytesIO(payload),
            ):
                self.assertEqual(fetch_locked_artifact("item", destination, lock_path), destination)
            self.assertEqual(destination.read_bytes(), payload)

            with mock.patch(
                "neptunesdr_firmwave.firmware.urllib.request.urlopen",
                return_value=io.BytesIO(payload + b"!"),
            ):
                with self.assertRaises(FirmwareFormatError):
                    fetch_locked_artifact("item", destination, lock_path)
            self.assertFalse((root / "item.bin.part").exists())


if __name__ == "__main__":
    unittest.main()
