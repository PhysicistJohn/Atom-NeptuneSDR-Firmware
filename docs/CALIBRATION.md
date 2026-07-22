# Calibration bundle and activation contract

Calibration is part of measurement state, not an invisible cleanup option.
The canonical storage description is `specs/neptune-calibration-bundle-v1.json`
and the validating implementation is `neptunesdr_firmwave.calibration`.

## Bundle identity

Each bundle is bound to one device serial and monotonic revision. Every table
names its channel mask, RF frequency interval, 61.44 MHz-or-lower sample rate,
RF bandwidth, analog-gain interval, temperature interval, calibration type,
and signed Q2.30 coefficient list. Supported table types cover DC offset, IQ
imbalance, channel amplitude/phase, frequency-response equalization, and
temperature compensation. The canonical-JSON SHA-256 covers all fields except
the digest field itself, and a bundle is limited to 262,144 coefficients.

The canonical table identity is the bundle device serial and revision plus
channel mask, both frequency endpoints, sample rate, bandwidth, both gain
endpoints, both temperature endpoints, and calibration type. Tables may
coexist when any one of those fields differs. Repeating the complete key fails
closed even when the coefficient arrays differ; coefficients are payload, not
identity.

Revision zero is the immutable factory/default starting point. A newer bundle
must name the currently active predecessor; skipped, repeated, foreign-device,
malformed, duplicate-key, oversized, or bad-integrity bundles fail closed.
Revision files are never overwritten. Store operations take an advisory
cross-process lock, fsync both each file and its parent directory, then replace
the active index atomically. A retry can reconcile an exact validated orphan
revision left by a crash between revision and index writes; different content
at that revision fails closed. The index binds the selected bundle digest to a
requested 61.44 MHz activation timestamp. Rollback selects an existing
validated revision and records a new activation timestamp without deleting
history.

## Runtime application contract (not yet integrated)

The target control daemon currently reports calibration commands as
`UNSUPPORTED`; this repository implements the validated persistent store and
standalone PL coefficient-bank contact, not their board-routed transaction.
The required integration is: coefficients are written only to an inactive PL
bank; the control service first
validates the complete upload and applicable RF-state key, then requests an
atomic bank swap at a sample or DSP-block boundary. It changes no active
coefficient on partial failure. The accepted transition produces:

- one monotonically updated calibration revision;
- one response and one asynchronous state-change event with the same ingress
  timestamp;
- the new revision on the first affected data packet;
- invalidity/discontinuity metadata if calibration activity interrupts valid
  sampling.

Raw bypass always reports calibration revision zero and never consumes these
tables. Each correction and EQ stage can be bypassed independently. Equalizer
generation must cap boost near response nulls; calibration tooling must retain
the raw reference capture, RF state, fixture identity, temperature, generator
settings, metrics and software/build hashes used to derive a table.

## Validation sequence

For a physical revision, use attenuated common-source TX-to-both-RX loopback and
measure raw versus corrected DC spur, image rejection, amplitude/phase error,
frequency response, EVM and temperature/gain drift. Sweep the exact table key
ranges and verify boundary interpolation. Re-run bypass equivalence after every
RTL change. A calibration revision is releasable only when its raw captures and
metric report are reproducible from the versioned toolchain.
