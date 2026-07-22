#!/usr/bin/env python3
"""Portable HDL source gate plus exact arithmetic reference checks."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "hdl" / "rtl" / "core"


def round_shift(value: int, shift: int) -> int:
    if shift == 0:
        return value
    magnitude = abs(value)
    quotient, remainder = divmod(magnitude, 1 << shift)
    half = 1 << (shift - 1)
    rounded = quotient + (remainder > half or (remainder == half and quotient & 1))
    return -rounded if value < 0 else rounded


def quantize_s8(value: int, shift: int) -> tuple[int, bool]:
    rounded = round_shift(value, shift)
    return max(-128, min(127, rounded)), not (-128 <= rounded <= 127)


def check_reference_math() -> None:
    cases = {
        (-32768, 8): (-128, False),
        (-32767, 8): (-128, False),
        (-129, 8): (-1, False),
        (-128, 8): (0, False),
        (-127, 8): (0, False),
        (-1, 8): (0, False),
        (0, 8): (0, False),
        (127, 8): (0, False),
        (128, 8): (0, False),
        (32767, 8): (127, True),
        (32767, 0): (127, True),
    }
    for key, expected in cases.items():
        actual = quantize_s8(*key)
        if actual != expected:
            raise AssertionError("quantizer %r produced %r, expected %r" % (key, actual, expected))

    accumulator = 0
    outputs = 0
    phases = set()
    for _ in range(1536):
        accumulator += 1375
        if accumulator >= 1536:
            accumulator -= 1536
            outputs += 1
            phases.add(accumulator)
    if outputs != 1375 or accumulator != 0:
        raise AssertionError("1375/1536 scheduler is not exact")
    if len(phases) != 1375:
        raise AssertionError("scheduler phase cycle is incomplete")


def check_source_contracts() -> None:
    required = {
        "neptune_sample_clock.sv": ("[63:0]", "discontinuity_pending", "counter_set_valid"),
        "neptune_rx_ingress.sv": ("dropped_samples", "overflow_sticky", "stream_ready"),
        "neptune_quantize_s8.sv": ("rounded_shift", "17'sd127", "-17'sd128"),
        "neptune_resampler_55m.sv": (".INTERPOLATION(125)", ".DECIMATION(128)", ".INTERPOLATION(11)", ".DECIMATION(12)"),
    }
    for name, needles in required.items():
        path = CORE / name
        source = path.read_text(encoding="utf-8")
        for needle in needles:
            if needle not in source:
                raise AssertionError("%s is missing %r" % (name, needle))


def optional_compile() -> str:
    iverilog = shutil.which("iverilog")
    if iverilog is None:
        return "SKIP (iverilog not installed)"
    source_paths = sorted((ROOT / "hdl" / "rtl").rglob("*.sv"))
    package_paths = [
        path for path in source_paths
        if "package " in path.read_text(encoding="utf-8")
    ]
    sources = [str(path) for path in package_paths + [path for path in source_paths if path not in package_paths]]
    testbenches = sorted((ROOT / "hdl" / "tb").glob("tb_*.sv"))
    with tempfile.TemporaryDirectory(prefix="neptune-hdl-") as temp:
        design_output = Path(temp) / "neptune_rx_pipeline.vvp"
        subprocess.run(
            [iverilog, "-g2012", "-s", "neptune_rx_pipeline", "-o", str(design_output), *sources],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=30,
        )
        for testbench in testbenches:
            top = testbench.stem
            output = Path(temp) / (top + ".vvp")
            defines = []
            if top == "tb_neptune_rx_pipeline":
                defines.append("-DNEPTUNE_SIM_FIR_MODEL")
            subprocess.run(
                [iverilog, "-g2012", *defines, "-s", top, "-o", str(output), *sources, str(testbench)],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=30,
            )
            subprocess.run(
                ["vvp", str(output)],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=30,
            )
    return "PASS"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    check_reference_math()
    check_source_contracts()
    compile_result = optional_compile()
    print("NEPTUNE_HDL_SOURCE PASS; compile=%s" % compile_result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
