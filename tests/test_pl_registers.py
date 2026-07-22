"""Canonical PL register map and generated-binding tests."""

from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "generate_registers.py"
SPEC = importlib.util.spec_from_file_location("generate_registers", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class PlRegisterTests(unittest.TestCase):
    def test_schema_and_safety_invariants(self):
        value = MODULE.load_validate()
        by_name = {item["name"]: item for item in value["registers"]}
        self.assertEqual(by_name["SAMPLE_COUNT_HI"]["offset"], by_name["SAMPLE_COUNT_LO"]["offset"] + 4)
        self.assertEqual(by_name["TX_PERSISTENT_INHIBIT"]["reset"], 1)
        self.assertEqual(by_name["STREAM0_CONTROL"]["reset"], 0)
        self.assertEqual(
            value["register_fields"]["STREAM0_FORMAT"]["SAMPLE_FORMAT"],
            {"lsb": 8, "width": 8},
        )
        register_file = (
            ROOT / "hdl" / "rtl" / "core" / "neptune_register_file.sv"
        ).read_text(encoding="utf-8")
        for name in by_name:
            with self.subTest(register=name):
                self.assertIn("REG_%s" % name, register_file)

    def test_generated_outputs_are_current_and_c_compiles(self):
        subprocess.run(["python3", str(SCRIPT), "--check"], cwd=ROOT, check=True, capture_output=True)
        self.assertEqual(
            (ROOT / "protocol" / "generated" / "neptune_pl_registers_v1.h").read_bytes(),
            (ROOT / "ps" / "include" / "neptune_pl_registers_v1.h").read_bytes(),
        )
        source = '#include "neptune_pl_registers_v1.h"\nint main(void){return NEPTUNE_PL_REG_TX_PERSISTENT_INHIBIT == 0x804 && NEPTUNE_PL_FIELD_STREAM0_FORMAT_SAMPLE_FORMAT_MASK == 0xff00U ? 0 : 1;}\n'
        with tempfile.TemporaryDirectory(prefix="neptune-registers-") as temporary:
            path = Path(temporary) / "test.c"
            path.write_text(source, encoding="utf-8")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "protocol" / "generated"), str(path), "-o", str(Path(temporary) / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
