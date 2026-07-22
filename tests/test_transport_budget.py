"""Wire-rate constraints for the 55 MSPS continuous profile."""

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PATH = ROOT / "tools" / "benchmark" / "transport_budget.py"
SPEC = importlib.util.spec_from_file_location("transport_budget", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class TransportBudgetTests(unittest.TestCase):
    def test_single_channel_55_msps_s8bf_requires_jumbo(self):
        standard = MODULE.transport_budget(55_000_000, "S8BF", 1, 1500)
        jumbo = MODULE.transport_budget(55_000_000, "S8BF", 1, 9000)
        self.assertFalse(standard["fits_gigabit"])
        self.assertTrue(jumbo["fits_gigabit"])
        self.assertEqual(jumbo["application_header_bytes"], 272)
        self.assertEqual(jumbo["samples_per_packet"], 4350)
        self.assertAlmostEqual(jumbo["wire_rate_bps"], 914_188_505.7471265)
        self.assertLess(jumbo["packets_per_second"], 13_000)

    def test_dual_channel_full_rate_never_fits_s8(self):
        result = MODULE.transport_budget(55_000_000, "S8", 2, 9000)
        self.assertFalse(result["fits_gigabit"])
        self.assertGreater(result["payload_rate_bps"], 1_000_000_000)

    def test_full_rate_raw_is_a_ddr_capture_product(self):
        raw = MODULE.transport_budget(61_440_000, "S16", 1, 9000)
        self.assertFalse(raw["fits_gigabit"])
        self.assertLess(raw["max_sample_rate_hz_at_line_rate"], 31_000_000)


if __name__ == "__main__":
    unittest.main()
