# Golden arithmetic v1

The single fixed-point arithmetic that every implementation of the P210
spectral neural operator reproduces bit-for-bit. This closes the root blocker
of the exact-twin program: previously three divergent arithmetics existed
(a float-FFT reference, the twin's Q2.30 CORDIC device, and an 18-bit-twiddle
RTL assumption), so no bit-exactness chain was possible.

**Status: implemented and proven three ways.** The normative reference is
`Atom-Neural-RL src/atom_neural_rl/golden.py`; the C core is
`Atom-NeptuneSDR-Twin cosim/operator-core/`; the RTL engine is
`Atom-NeptuneSDR-Twin cosim/operator-rtl/`. All three reproduce the pinned
test-vector digests below exactly. Verification commands are at the end.

## Pinned decisions

| Item | Decision |
|---|---|
| Data | 24-bit signed complex components, carried in int64 (C/Python) / 24-bit regs (RTL). IQ16 input enters sign-extended, left-shifted 8. |
| Twiddles | 18-bit signed Q1.17 from the **committed ROM**: 32768 (cos, sin) pairs over theta in [0, pi) at 2pi/65536 resolution, int32 little-endian interleaved. sha256 `12b5bcfe886e72c759c3a231a46283fc2c4fddadb9fdad92325e1eb8026100f7`. Implementations load the artifact; nothing recomputes twiddles, so host libm can never perturb the chain. FFT twiddle W_N^k = (cos, -sin) at ROM index `k * (65536/N)`; for stage `s`, position `j`, index = `j << (15 - s)`. |
| Rounding | ONE rule everywhere: **round-half-to-even right shift** (`rhe`). Ties go to the even quotient. C and Verilog implement the identical function; C division and Verilog `>>>` are never used bare on signed values. This designs out the C-truncate vs Verilog-floor negative-halving mismatch. |
| FFT | Radix-2 DIT, bit-reversal on entry, natural-order output, `rhe(.,1)` per stage; forward computes FFT(x)/N. Twiddle products rounded `rhe(.,17)`. Outputs clamped to 24-bit. |
| IFFT | conjugate -> forward -> conjugate. Exactly the true IFFT (the forward's 1/N is the IFFT's own 1/N). |
| Block exponent | The spectral round trip IFFT(H . FFT(x)) carries a net 1/N: block exponent += log2(N) per round trip, plus the table exponent. Sample containers stay 24-bit; the exponent is result metadata (TELEM_BLOCK_EXP). |
| Spectral multiply | Tables are int16 Q1.15 mantissas + one shared int8 power-of-two exponent; `Y = rhe(X*H, 15)`, clamp 24-bit. |
| Backbone | lift/mix/project entries are complex int16 Q1.15 (round-half-even at bank compile); int64 accumulate across inputs, one `rhe(.,15)` + clamp per output. The mix path (pre-FFT scale) is aligned to the spectral path by `rhe`-shifting down by the round trip's exponent gain before the sum. |
| modReLU | Magnitude by alpha-max-beta-min: `m = rhe(15*max,4) + rhe(15*min,5)` (alpha 15/16, beta 15/32). Threshold `b` is a non-positive Q1.23 integer at block exponent 0, shifted to the current block exponent before comparison (`rhe` down for positive exponents, exact left shift for negative) -- the gate decision is block-scale independent by construction. Survivor scale `s = ((m+b) << 15) / m` with floor division of non-negative operands (identical in C `/` and Verilog `/`). Output `rhe(z*s, 15)`, clamp. `b = 0` gives `s = 2^15` and exact identity. |
| Vector PRNG | splitmix64 (constants 0x9E3779B97F4A7C15, 0xBF58476D1CE4E5B9, 0x94D049BB133111EB); test input takes the low 32 bits of each draw as two signed 16-bit words, `<< 8`. |

## Pinned cross-implementation vectors

sha256 over int32-LE serialization of (re || im) output:

| Case | seed | N | digest |
|---|---|---|---|
| fft | 11 | 256 | `ccea7a8301f8b8372bb1d2365b4e06e7330200ffee249e0f1634210fb9dd7a22` |
| fft | 12 | 1024 | `09ea67d3e313581054387508d4709b2dc6b04cdc26d2a5036dd024a4037b82b1` |
| fft | 13 | 4096 | `481b10ecbe9823d42e3e279ca88f76aba2e7e2e7229926cb3bd81edfc8f8aa80` |
| roundtrip (H=0.5 flat) | 21 | 1024 | `cd276c1485d97ce30b707aa9a9cd08d4c6f8170333020e0413f740a29bfcc0d2` |

## Relationship to the v1 FFT device

The shipped v1 power-FFT device (Q2.30 CORDIC, truncate-toward-zero) is
**unchanged and remains the v1 contract**: reset state is bit-exact v1, and the
v1 acceptance gate still passes unmodified. Golden-arithmetic v1 governs the
**operator mode only** (the v2 capability surface). The operator datapath in
the twin and the RTL is this arithmetic; the legacy power path stays Q2.30
until/unless a major revision retires it.

## What this closes and what stays open

Closed: the three-way arithmetic divergence; the C-vs-RTL rounding landmine;
libm/tie sensitivity in twiddles (committed ROM); the reference model's float
FFT (superseded by `GoldenExecutor` for all bit-exactness evidence); and the
proof method (shared pinned vectors, all three implementations).

Open, tracked in the gap register: Vivado resource/timing closure of the RTL
on xc7z020 alongside the radio; the QEMU MMIO device wrapper around the C core
(the compute core is the proven part); large-N (> 2^12 sim, > BRAM working set)
datapath architecture; and everything hardware.

## Verification

```
# Python reference + pins
cd Atom-Neural-RL && PYTHONPATH=src python3 -m unittest tests.test_golden

# C core == golden
cd Atom-NeptuneSDR-Twin/cosim/operator-core
cc -O2 -o test_operator_core p210_operator_core.c test_operator_core.c
./test_operator_core <path>/twiddle-rom-q117.bin

# RTL == golden (iverilog)
cd Atom-NeptuneSDR-Twin/cosim/operator-rtl
iverilog -g2012 -o tb_fft p210_fft_engine.v tb_fft_engine.v && ./tb_fft
```
