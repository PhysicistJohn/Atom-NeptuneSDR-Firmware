# Vivado 2023.2 batch worker

The physical P210 build is pinned to Vivado ML Standard 2023.2 because the
newer public hardware handoff was produced by that release. AMD supports that
release on x86/x86-64 Windows and Linux, not ARM macOS. The XC7Z020 is covered
by the no-cost Standard edition.

`lima-vivado-2023.2.yaml` creates an Ubuntu 22.04 x86-64 batch worker. On an
Apple Silicon host it is a correctness fallback using QEMU system emulation,
not a performance recommendation. Prefer a native x86-64 self-hosted worker
for implementation and timing closure.

The VM deliberately does not embed or download proprietary AMD software.
After accepting AMD's terms, place the Linux 2023.2 web installer in a host
tool cache and use the repository installer/doctor to install only Vivado,
Vitis Embedded, Bootgen, and Zynq-7000 device support. Never commit the
installer or installed suite.

The worker identity is recorded in `/etc/neptune-vivado-worker`. A release
build must capture `vivado -version`, IP catalog versions, OS identity, and
SHA-256 hashes for every generated report and artifact.

After committing an exact source state, transfer it without a shared host
mount:

```sh
tools/vivado/vm.sh sync
```

`sync` refuses a dirty tree, creates a Git bundle, verifies the bundle digest
inside the guest, checks out the full commit in a new immutable path under
`/opt/neptune-build`, and refuses to overwrite a previous source target.
