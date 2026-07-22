# Board constraints gate

No XDC is intentionally guessed for `p210-edge-custom-v0`. The public Neptune
artifacts disagree about the AD9361 digital interface, DDR width/capacity,
Ethernet/USB topology, GPIO, and even source-tool identity. Pin locations,
I/O standards, clock waveforms, generated clocks, false paths, and asynchronous
clock groups must come from one revision-matched board handoff and physical
reconnaissance record.

A physical build remains blocked until this directory contains, at minimum:

- the board pin/I/O constraint file tied to a PCB revision;
- primary AD9361 RX, PS, Ethernet, and reference-clock definitions;
- generated-clock constraints for every derived PL clock;
- reviewed CDC exceptions with source/destination synchronizer evidence;
- reset, TX-safe-state, and unconstrained-port checks;
- an identity file hashing the matched schematic/hardware export.

The build must fail if Vivado reports an unconstrained endpoint, critical CDC,
or XDC whose hardware identity differs from the selected manifest. A comment-
only placeholder XDC would weaken that gate, so none is supplied.
