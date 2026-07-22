"""Release artifacts retain the operational, licensing, and provenance sources."""

import importlib.util
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
VERIFY_PATH = ROOT / "tools" / "release" / "verify_distribution.py"
VERIFY_SPEC = importlib.util.spec_from_file_location(
    "neptune_verify_distribution", VERIFY_PATH
)
if VERIFY_SPEC is None or VERIFY_SPEC.loader is None:  # pragma: no cover
    raise RuntimeError("cannot load distribution verifier")
VERIFY_DISTRIBUTION = importlib.util.module_from_spec(VERIFY_SPEC)
VERIFY_SPEC.loader.exec_module(VERIFY_DISTRIBUTION)


def _declared_profile_data_files():
    """Return source paths and installed wheel suffixes from pyproject.toml."""

    sources = []
    wheel_suffixes = []
    destination = None
    in_data_files = False
    for raw in (ROOT / "pyproject.toml").read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line == "[tool.setuptools.data-files]":
            in_data_files = True
            continue
        if in_data_files and line.startswith("["):
            break
        if not in_data_files or not line:
            continue
        match = re.fullmatch(r'"([^"]+)"\s*=\s*\["([^"]+)"\]', line)
        if match:
            source = match.group(2)
            sources.append(source)
            wheel_suffixes.append(
                match.group(1) + "/" + source.rsplit("/", 1)[-1]
            )
            destination = None
            continue
        match = re.fullmatch(r'"([^"]+)"\s*=\s*\[', line)
        if match:
            destination = match.group(1)
            continue
        if line == "]":
            destination = None
            continue
        match = re.fullmatch(r'"([^"]+)",?', line)
        if match and destination is not None:
            source = match.group(1)
            sources.append(source)
            wheel_suffixes.append(destination + "/" + source.rsplit("/", 1)[-1])
    return tuple(sources), tuple(wheel_suffixes)


class DistributionTests(unittest.TestCase):
    def test_wheel_verifier_covers_every_declared_profile_data_file(self):
        sources, wheel_suffixes = _declared_profile_data_files()
        self.assertTrue(sources)
        self.assertEqual(
            set(VERIFY_DISTRIBUTION.DECLARED_PROFILE_SOURCE_SUFFIXES), set(sources)
        )
        self.assertEqual(
            set(VERIFY_DISTRIBUTION.DECLARED_PROFILE_WHEEL_SUFFIXES),
            set(wheel_suffixes),
        )
        self.assertEqual(len(sources), len(set(sources)))
        self.assertEqual(len(wheel_suffixes), len(set(wheel_suffixes)))
        self.assertEqual(
            len(VERIFY_DISTRIBUTION.DECLARED_PROFILE_SOURCE_SUFFIXES),
            len(set(VERIFY_DISTRIBUTION.DECLARED_PROFILE_SOURCE_SUFFIXES)),
        )
        self.assertEqual(
            len(VERIFY_DISTRIBUTION.DECLARED_PROFILE_WHEEL_SUFFIXES),
            len(set(VERIFY_DISTRIBUTION.DECLARED_PROFILE_WHEEL_SUFFIXES)),
        )
        self.assertTrue(set(sources) <= set(VERIFY_DISTRIBUTION.SDIST_SUFFIXES))
        for source in sources:
            with self.subTest(path=source):
                self.assertTrue((ROOT / source).is_file())

    def test_wheel_verifier_covers_all_declared_package_profile_json(self):
        expected = {
            "neptunesdr_firmwave/data/" + path.name
            for path in (ROOT / "src" / "neptunesdr_firmwave" / "data").glob("*.json")
        }
        self.assertTrue(expected)
        self.assertTrue(expected <= set(VERIFY_DISTRIBUTION.PACKAGE_WHEEL_SUFFIXES))
        self.assertTrue(expected <= set(VERIFY_DISTRIBUTION.SDIST_SUFFIXES))

    def test_missing_provenance_document_is_rejected(self):
        provenance = "share/neptunesdr-firmwave/docs/PROVENANCE.md"
        names = tuple(
            "archive-root/" + suffix
            for suffix in VERIFY_DISTRIBUTION.WHEEL_SUFFIXES
            if suffix != provenance
        )
        with self.assertRaisesRegex(ValueError, re.escape(provenance)):
            VERIFY_DISTRIBUTION._require_suffixes(
                names, VERIFY_DISTRIBUTION.WHEEL_SUFFIXES, "wheel"
            )

    def test_sdist_manifest_covers_every_nonpackage_source_class(self):
        expected = {
            "include LICENSE",
            "include NOTICE.md",
            "include README.md",
            "include pyproject.toml",
            "recursive-include board *.env *.json *.md",
            "recursive-include LICENSES *.txt",
            "recursive-include docs *.md",
            "recursive-include firmware *.c",
            "recursive-include hdl *.md *.py *.sv",
            "recursive-include protocol *.h *.json *.md *.py",
            "recursive-include ps *.c *.h *.md *.py *.yaml Kconfig Makefile",
            "recursive-include scripts *.md *.py *.sh",
            "recursive-include specs *.json",
            "recursive-include tools *.md *.py *.sh *.yaml Makefile",
        }
        directives = {
            line.strip()
            for line in (ROOT / "MANIFEST.in").read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
        self.assertEqual(directives, expected)

        required = (
            "LICENSES/BSD-2-Clause.txt",
            "LICENSES/GPL-2.0-or-later.txt",
            "board/p210/build-plan.json",
            "board/p210/profile.json",
            "board/p210/source-lock.json",
            "board/p210/toolchain-lock.json",
            "docs/ACCEPTANCE.md",
            "docs/BOARD_BUILD.md",
            "docs/CALIBRATION.md",
            "docs/P210_FFT_ABI.md",
            "docs/HARDWARE_EVIDENCE.md",
            "docs/NEPTUNE_EDGE_ARCHITECTURE.md",
            "docs/PROVENANCE.md",
            "docs/RECOVERY.md",
            "firmware/neptune_fft_streamer.c",
            "hdl/rtl/core/neptune_sample_clock.sv",
            "protocol/generated/neptune_edge_v1.h",
            "protocol/generated/neptune_edge_v1.py",
            "protocol/generated/neptune_edge_profile_v1.json",
            "protocol/golden/neptune_edge_v1_vectors.json",
            "ps/include/neptune_pl_registers_v1.h",
            "ps/include/neptune_stream_uapi.h",
            "scripts/board_doctor.py",
            "scripts/build_board.py",
            "scripts/build_bundle.sh",
            "scripts/fetch_firmware.py",
            "scripts/generate_protocol.py",
            "scripts/generate_edge_profile.py",
            "scripts/request_vendor_materials.md",
            "tools/host/Makefile",
            "tools/release/verify_distribution.py",
            "specs/neptune-edge-control-v1.json",
            "specs/neptune-edge-data-v1.json",
            "specs/neptune-calibration-bundle-v1.json",
            "specs/neptune-update-manifest-v1.json",
            "specs/p210-firmware-interface-v1.json",
            "specs/p210-hardware-manifest-v1.schema.json",
            "src/neptunesdr_firmwave/data/p210-hardware-candidate-v1.json",
        )
        for relative in required:
            with self.subTest(path=relative):
                self.assertTrue((ROOT / relative).is_file())
