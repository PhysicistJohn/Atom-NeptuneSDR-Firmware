# Neptune Edge programmable-logic data plane

This directory is the source-owned RTL for the `hardware/custom-v0` profile.
It is deliberately independent of the legacy NSFT/QEMU compatibility service.

The receive clock domain has one non-negotiable rule: `sample_tick` represents
an AD9361 complex-sample period and cannot be backpressured. Every stage that
crosses into an AXI-stream or DMA domain therefore reports overflow rather
than silently stalling, overwriting, interpolating, or renumbering samples.

The first physical proof path is:

```text
ADI axi_ad9361 / util_cpack2 contact
  -> neptune_rx_ingress (64-bit sample counter and raw tap)
  -> correction stages (all independently bypassable)
  -> two-stage filtered resampler (125/128 then 11/12; 61.44 -> 60 -> 55 MSPS)
  -> quantizer / block statistics
  -> protocol packet buffer
  -> ADI AXI-DMAC -> dedicated DDR ring -> PS GEM0
```

The raw tap is always before correction and rate conversion. Full-rate raw
samples are available to the DDR trigger path even when continuous Ethernet
uses the 55 MSPS output. The factors are split because the target FIR IP only
supports rate-change factors through 1024. The resampler is never replaced by
periodic sample deletion; its coefficient images are generated and audited as
build artifacts.

The custom-v0 S8BF baseline is peak-scaled, round-to-nearest-even, with one
headroom bit (target absolute code 63). Its metadata exponent is the negative
of the applied power-of-two shift, so reconstruction is exactly
`quantized_code * 2^exponent` in ADC-code units.

Fixed/S8BF selection in `neptune_rx_pipeline` is latched at a completed
resampler transfer: a stalled beat cannot be rerouted by a mode request. An
S8BF-to-fixed transition also closes any partial S8BF block before later
samples can be accumulated. Each completed block retains the first sample's
integer and Q32 fractional source time and ORs the discontinuity state of every
sample in that block. Those descriptor fields remain stable under output
backpressure and across revision-driven or partial-block closes. The portable
pipeline bench uses an explicitly test-only transaction model for the vendor
FIR contact; it checks the exact 1375/1536 rate, ordering, ready/valid behavior,
and metadata continuity, not the coefficient-level frequency response of
generated Xilinx FIR IP.

## Layout

- `rtl/core`: board-independent synthesizable processing blocks.
- `rtl/vendor`: narrow wrappers around ADI/Xilinx IP contacts.
- `tb`: self-checking simulation testbenches.
- `constraints`: fail-closed constraint requirements; actual XDC remains absent
  until a revision-matched board handoff is reconciled.
- `scripts`: deterministic coefficient/IP generation.

`scripts/check_hdl.py` always runs portable structural/reference checks. When
Icarus Verilog or Verilator is installed it also compiles the RTL and executes
the self-checking testbenches. Vivado synthesis, implementation, timing, CDC,
and DRC reports are mandatory release artifacts, not optional documentation.

## Clock and reset contracts

- The sample counter lives in the AD9361 receive sample clock domain.
- Counter changes require an explicit `counter_set_valid` command.
- A reset, retune, interface error, dropped sample, or counter set causes a
  discontinuity flag on a sample and a state-change record in the metadata
  path.
- No correction stage is allowed to alter sample timestamps.
- Configuration shadow registers commit only on `sample_tick` boundaries.

## TX safety

The custom-v0 top-level does not instantiate an RF transmit data path. Future
TX RTL must have independent HDL, PS, device-tree, and persistent inhibit gates
and must remain disabled after every reset until an authenticated, explicit
enable transaction succeeds.
