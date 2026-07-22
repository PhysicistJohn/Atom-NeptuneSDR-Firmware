# Custom-v0 acceptance ledger

This ledger prevents source-level conformance from being mistaken for physical
validation. `source-pass` means deterministic code/simulation tests exist in
this repository. `hardware-required` means the same criterion must still be
measured on the reconciled board and routed bitstream before release.

| Criterion | Source status | Physical release evidence |
|---|---|---|
| Dual RX sample alignment | partial: dual-channel ingress arithmetic is simulated; no board-routed AD9361 interface exists | common-source phase/alignment sweep on RX1/RX2 |
| USB control during saturated Ethernet | independent daemon/data-service paths implemented | latency and disconnect test at sustained jumbo line load |
| Stream ID, sequence, timestamp | NEDP schema, builders, golden vectors and Twin decoder pass | packet capture from the routed PL/DMA/GEM path |
| Observable loss/retune/restart | discontinuity revisions, causes, sticky counters and host tracker pass | induced FIFO/DMA/link/retune fault matrix |
| Native raw bypass | pre-correction contact and bypass assertions present | compare ADC contact to captured S16/S12P bytes |
| S8 and S8BF | nearest-even/saturation, block scale/stats, reconstruction, Q32 first-sample time, aggregate discontinuity, pipeline mux/backpressure, revision split and partial-block flush tests pass | coefficient-accurate Vivado simulation plus EVM, SNR, spectral error and clipping characterization |
| Independent correction bypass | partial: DC/IQ arithmetic and bypasses pass; EQ has coefficient/control contacts but no FIR data path | raw-vs-each-stage loopback comparison |
| Timestamped coefficient update | partial: shadow banks swap at a block boundary; requested-timestamp enforcement is not integrated | update while streaming and verify first affected sample |
| Concurrent IQ plus FFT/STFT | window/STFT scheduling contacts exist; FFT vendor core/top-level integration pending | timing-closed concurrent product throughput capture |
| Triggered DDR pre/post capture | trigger controller exists; board DDR circular writer pending | known timestamp/software/detector trigger capture |
| Reproducible build | generators, locks, VM definition and portable source gate pass | same hashes/reports from a clean Vivado/rootfs build |
| Full identity reporting | fixed identity layouts, profile hashes, PL status UAPI, and control contacts exist | host report containing hardware/FPGA/PS/calibration/protocol IDs |

Additional source gates cover signed removable-SD A/B update admission,
rollback resistance, QSPI prohibition, TX-safe reset state, malformed USB
frames, stale DMA slots, exact 61.44-to-55 MHz sample-time mapping, and Twin
capture/replay compatibility.

The custom profile is releasable only when every `hardware-required` cell has a
versioned artifact tied to the same board serial, source commit, bitstream build
ID, Vivado report set, PS image manifest, and protocol/calibration revisions.
