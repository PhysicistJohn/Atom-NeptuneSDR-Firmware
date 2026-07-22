"""Portable source and arithmetic tests for the custom PL data plane."""

from pathlib import Path
import importlib.util
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_script(name: str):
    path = ROOT / "hdl" / "scripts" / name
    spec = importlib.util.spec_from_file_location(name.replace(".py", ""), path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class HdlSourceTests(unittest.TestCase):
    def test_reference_arithmetic_and_source_contracts(self):
        checker = load_script("check_hdl.py")
        checker.check_reference_math()
        checker.check_source_contracts()

    def test_resampler_coefficients_are_exact_and_audited(self):
        generator = load_script("generate_resampler.py")
        with tempfile.TemporaryDirectory(prefix="neptune-coeff-") as temporary:
            manifest = generator.generate(Path(temporary))
            generator.validate_manifest(manifest)
            self.assertEqual(manifest["internal_sample_rate_hz"], 61_440_000)
            self.assertEqual(manifest["transport_sample_rate_hz"], 55_000_000)
            self.assertEqual(manifest["overall_interpolation"], 1375)
            self.assertEqual(manifest["overall_decimation"], 1536)
            self.assertEqual([stage["name"] for stage in manifest["stages"]], ["125_128", "11_12"])

    def test_raw_path_has_no_silent_drop_construct(self):
        source = (ROOT / "hdl" / "rtl" / "core" / "neptune_rx_ingress.sv").read_text(encoding="utf-8")
        self.assertIn("dropped_samples       <= dropped_samples + 64'd1", source)
        self.assertIn("discontinuity_pending <= 1'b1", source)


if __name__ == "__main__":
    unittest.main()
