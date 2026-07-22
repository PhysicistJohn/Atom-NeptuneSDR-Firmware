"""Fail-closed checks for the reproducible Vivado worker definition."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class VivadoWorkerTests(unittest.TestCase):
    def test_worker_is_pinned_x86_ubuntu_and_headless(self):
        source = (ROOT / "tools" / "vivado" / "lima-vivado-2023.2.yaml").read_text(encoding="utf-8")
        self.assertIn("vmType: qemu", source)
        self.assertIn("arch: x86_64", source)
        self.assertIn("plain: true", source)
        self.assertIn("ubuntu-22.04-server-cloudimg-amd64.img", source)
        self.assertIn("sha256:ec3cdc1bf496078f645ccc8ac823e17609658753477ebc4e5fb730729ac5b434", source)
        self.assertIn("vivado-required=2023.2", source)

    def test_installer_requires_explicit_license_and_digest(self):
        source = (ROOT / "tools" / "vivado" / "install-vivado-2023.2.sh").read_text(encoding="utf-8")
        self.assertIn("AMD_EULA_ACCEPTED", source)
        self.assertIn("sha256sum", source)
        self.assertIn("--batch Install", source)
        self.assertIn("Vivado v2023\\.2", source)
        self.assertNotIn("curl ", source)
        self.assertNotIn("wget ", source)

    def test_source_sync_is_clean_content_addressed_and_non_overwriting(self):
        source = (ROOT / "tools" / "vivado" / "vm.sh").read_text(encoding="utf-8")
        self.assertIn("status --porcelain=v1 --untracked-files=all", source)
        self.assertIn("git -C \"$ROOT\" bundle create", source)
        self.assertIn("bundle_sha256=", source)
        self.assertIn("immutable guest source target already exists", source)
        self.assertNotIn("mount", source)


if __name__ == "__main__":
    unittest.main()
