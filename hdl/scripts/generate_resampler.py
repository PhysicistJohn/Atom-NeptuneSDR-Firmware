#!/usr/bin/env python3
"""Generate and audit fixed-point coefficients for the 55 MSPS egress path."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
from typing import Dict, Iterable, List, Sequence


SCHEMA = "neptune.resampler-coefficients/v1"
COEFFICIENT_BITS = 18
COEFFICIENT_FRACTION_BITS = 17
ATTENUATION_DB = 80.0

STAGES = (
    {
        "name": "125_128",
        "input_rate_hz": 61_440_000,
        "output_rate_hz": 60_000_000,
        "interpolation": 125,
        "decimation": 128,
        "passband_hz": 25_000_000,
        "stopband_hz": 30_000_000,
    },
    {
        "name": "11_12",
        "input_rate_hz": 60_000_000,
        "output_rate_hz": 55_000_000,
        "interpolation": 11,
        "decimation": 12,
        "passband_hz": 25_000_000,
        "stopband_hz": 27_500_000,
    },
)


def _bessel_i0(value: float) -> float:
    total = 1.0
    term = 1.0
    index = 1
    half_squared = (value * value) / 4.0
    while True:
        term *= half_squared / (index * index)
        previous = total
        total += term
        if total == previous or abs(term) < abs(total) * 1e-16:
            return total
        index += 1


def _kaiser_beta(attenuation_db: float) -> float:
    if attenuation_db > 50.0:
        return 0.1102 * (attenuation_db - 8.7)
    if attenuation_db >= 21.0:
        delta = attenuation_db - 21.0
        return 0.5842 * delta ** 0.4 + 0.07886 * delta
    return 0.0


def _tap_count(stage: Dict[str, int], attenuation_db: float) -> int:
    interpolation = stage["interpolation"]
    upsampled_rate = stage["input_rate_hz"] * interpolation
    transition = stage["stopband_hz"] - stage["passband_hz"]
    delta_omega = 2.0 * math.pi * transition / upsampled_rate
    estimate = int(math.ceil((attenuation_db - 8.0) / (2.285 * delta_omega))) + 1
    # Equal-length polyphase branches make implementation and auditing simpler.
    return int(math.ceil(estimate / interpolation)) * interpolation


def design_stage(stage: Dict[str, int], attenuation_db: float = ATTENUATION_DB) -> List[float]:
    interpolation = stage["interpolation"]
    upsampled_rate = stage["input_rate_hz"] * interpolation
    cutoff = (stage["passband_hz"] + stage["stopband_hz"]) / 2.0
    taps = _tap_count(stage, attenuation_db)
    beta = _kaiser_beta(attenuation_db)
    denominator = _bessel_i0(beta)
    center = (taps - 1) / 2.0
    coefficients: List[float] = []
    for index in range(taps):
        offset = index - center
        argument = 2.0 * cutoff * offset / upsampled_rate
        sinc = 1.0 if argument == 0.0 else math.sin(math.pi * argument) / (math.pi * argument)
        ideal = 2.0 * cutoff / upsampled_rate * sinc
        position = (2.0 * index / (taps - 1)) - 1.0
        window = _bessel_i0(beta * math.sqrt(max(0.0, 1.0 - position * position))) / denominator
        coefficients.append(ideal * window)

    # A zero-insertion interpolator needs total gain L. This also makes the
    # average DC gain of each polyphase branch exactly one before quantization.
    gain = interpolation / sum(coefficients)
    return [value * gain for value in coefficients]


def quantize(coefficients: Sequence[float]) -> List[int]:
    scale = 1 << COEFFICIENT_FRACTION_BITS
    minimum = -(1 << (COEFFICIENT_BITS - 1))
    maximum = (1 << (COEFFICIENT_BITS - 1)) - 1
    result = []
    for coefficient in coefficients:
        value = int(math.floor(coefficient * scale + 0.5)) if coefficient >= 0 else -int(
            math.floor(-coefficient * scale + 0.5)
        )
        if value < minimum or value > maximum:
            raise ValueError("coefficient does not fit signed Q1.17")
        result.append(value)
    return result


def _response(coefficients: Sequence[int], rate_hz: int, frequency_hz: float) -> complex:
    scale = float(1 << COEFFICIENT_FRACTION_BITS)
    step = -2.0 * math.pi * frequency_hz / rate_hz
    return sum(
        (value / scale) * complex(math.cos(step * index), math.sin(step * index))
        for index, value in enumerate(coefficients)
    )


def audit(stage: Dict[str, int], coefficients: Sequence[int]) -> Dict[str, object]:
    interpolation = stage["interpolation"]
    upsampled_rate = stage["input_rate_hz"] * interpolation
    dc = abs(_response(coefficients, upsampled_rate, 0.0))
    passband_points = (0.0, stage["passband_hz"] * 0.5, stage["passband_hz"])
    stopband_points = (
        stage["stopband_hz"],
        (stage["stopband_hz"] + stage["input_rate_hz"] / 2.0) / 2.0,
        stage["input_rate_hz"] / 2.0,
    )
    passband_db = [20.0 * math.log10(max(abs(_response(coefficients, upsampled_rate, f)) / dc, 1e-20)) for f in passband_points]
    stopband_db = [20.0 * math.log10(max(abs(_response(coefficients, upsampled_rate, f)) / dc, 1e-20)) for f in stopband_points]
    phase_sums = [sum(coefficients[phase::interpolation]) / float(1 << COEFFICIENT_FRACTION_BITS) for phase in range(interpolation)]
    return {
        "dc_gain": dc,
        "passband_points_hz": list(passband_points),
        "passband_db": passband_db,
        "stopband_points_hz": list(stopband_points),
        "stopband_db": stopband_db,
        "polyphase_dc_min": min(phase_sums),
        "polyphase_dc_max": max(phase_sums),
    }


def _coefficient_text(coefficients: Iterable[int]) -> str:
    mask = (1 << COEFFICIENT_BITS) - 1
    return "\n".join(format(value & mask, "05x") for value in coefficients) + "\n"


def _coe_text(coefficients: Iterable[int]) -> str:
    values = [str(value) for value in coefficients]
    return "radix=10;\ncoefdata=\n" + ",\n".join(values) + ";\n"


def generate(output_dir: Path) -> Dict[str, object]:
    output_dir.mkdir(parents=True, exist_ok=True)
    reports = []
    for raw_stage in STAGES:
        stage = dict(raw_stage)
        floating = design_stage(stage)
        fixed = quantize(floating)
        mem_text = _coefficient_text(fixed)
        coe_text = _coe_text(fixed)
        mem_path = output_dir / ("resampler_%s.mem" % stage["name"])
        coe_path = output_dir / ("resampler_%s.coe" % stage["name"])
        mem_path.write_text(mem_text, encoding="ascii")
        coe_path.write_text(coe_text, encoding="ascii")
        report = {
            **stage,
            "coefficient_bits": COEFFICIENT_BITS,
            "coefficient_fraction_bits": COEFFICIENT_FRACTION_BITS,
            "coefficient_count": len(fixed),
            "taps_per_phase": len(fixed) // stage["interpolation"],
            "mem_sha256": hashlib.sha256(mem_text.encode("ascii")).hexdigest(),
            "coe_sha256": hashlib.sha256(coe_text.encode("ascii")).hexdigest(),
            "audit": audit(stage, fixed),
        }
        reports.append(report)

    manifest = {
        "schema": SCHEMA,
        "internal_sample_rate_hz": 61_440_000,
        "transport_sample_rate_hz": 55_000_000,
        "overall_interpolation": 1375,
        "overall_decimation": 1536,
        "passband_hz": 25_000_000,
        "attenuation_target_db": ATTENUATION_DB,
        "stages": reports,
    }
    manifest_path = output_dir / "resampler-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def validate_manifest(manifest: Dict[str, object]) -> None:
    if manifest["overall_interpolation"] * manifest["internal_sample_rate_hz"] != manifest["overall_decimation"] * manifest["transport_sample_rate_hz"]:
        raise ValueError("overall resampler ratio is not exact")
    for stage in manifest["stages"]:
        audit_data = stage["audit"]
        if max(abs(value) for value in audit_data["passband_db"]) > 0.20:
            raise ValueError("%s passband ripple exceeds 0.20 dB at audit points" % stage["name"])
        if max(audit_data["stopband_db"]) > -70.0:
            raise ValueError("%s stopband attenuation is below 70 dB at audit points" % stage["name"])
        if abs(audit_data["polyphase_dc_min"] - 1.0) > 0.01 or abs(audit_data["polyphase_dc_max"] - 1.0) > 0.01:
            raise ValueError("%s phase DC gain is out of bounds" % stage["name"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=Path("build/hdl/resampler"))
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    manifest = generate(args.output_dir)
    validate_manifest(manifest)
    if args.json:
        print(json.dumps(manifest, indent=2, sort_keys=True))
    else:
        print("NEPTUNE_RESAMPLER_COEFFICIENTS PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
