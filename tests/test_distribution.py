"""Release artifacts retain the operational, licensing, and provenance sources."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class DistributionTests(unittest.TestCase):
    def test_sdist_manifest_covers_every_nonpackage_source_class(self):
        expected = {
            "include LICENSE",
            "include NOTICE.md",
            "include README.md",
            "include pyproject.toml",
            "recursive-include LICENSES *.txt",
            "recursive-include docs *.md",
            "recursive-include firmware *.c",
            "recursive-include scripts *.md *.py *.sh",
            "recursive-include specs *.json",
        }
        directives = {
            line.strip()
            for line in (ROOT / "MANIFEST.in").read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
        self.assertEqual(directives, expected)

        required = (
            "LICENSES/GPL-2.0-or-later.txt",
            "docs/P210_FFT_ABI.md",
            "docs/PROVENANCE.md",
            "firmware/neptune_fft_streamer.c",
            "scripts/build_bundle.sh",
            "scripts/fetch_firmware.py",
            "scripts/request_vendor_materials.md",
            "specs/p210-firmware-interface-v1.json",
        )
        for relative in required:
            with self.subTest(path=relative):
                self.assertTrue((ROOT / relative).is_file())
