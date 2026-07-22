# Safe update and recovery model

The custom profile boots only from removable SD during bring-up. Factory media
must be imaged and stored before inserting custom media. There is no repository
command that writes a block device or QSPI, and the board definition rejects
such stages.

## A/B update contract

`specs/neptune-update-manifest-v1.json` and
`neptunesdr_firmwave.update_manifest` define the admission gate. A candidate
must target platform `p210-xc7z020-custom-v0`, name inactive slot A or B, require
boot and rootfs artifacts, explicitly forbid QSPI updates, advance the accepted
64-bit rollback index, and pass exact size/SHA-256 checks. Its canonical JSON is
verified with a provisioned Ed25519 public key. Paths are relative, contained,
regular files; duplicate canonical paths, roles, or resolved file identities,
traversal, symlinks and malformed signatures fail closed.

Admission opens the artifact root, each relative parent directory, and each
final artifact with no-follow, descriptor-relative operations. It rejects a
symlinked root or immediate root parent, any symlinked manifest parent/final
component, non-regular files, and different paths that are hard links to the
same device/inode. Hashing uses the opened descriptor and compares device,
inode, mode, size, mtime and ctime before and after the read. The trusted public
key's immediate parent and final file are also opened without following
symlinks; the bounded key bytes are snapshotted into the private verification
directory before invoking OpenSSL.

This closes path swaps during one admission call but cannot bind a later writer
after the admission descriptors have been closed. A privileged updater must
consume still-pinned descriptors or copy into private content-addressed staging
and recheck the signed digest immediately before writing and during readback.

An eventual privileged updater must follow this state machine:

1. Verify manifest, trusted-key identity, rollback index and every artifact.
2. Confirm TX is disarmed and persistent inhibit is asserted.
3. Resolve the inactive removable-SD slot by stable partition identity.
4. Stage and read back all files without touching the active slot or QSPI.
5. Write a pending boot record with a bounded attempt count.
6. Reboot; the watchdog considers the candidate healthy only after PL ABI,
   AD9361, USB control and required telemetry checks pass.
7. Promote the slot and persist its rollback index only after the health mark.
8. On timeout/crash, return to the last healthy slot and preserve fault logs.

The current tree implements admission verification, not the privileged block-
device writer. That writer is intentionally blocked until partition identities,
U-Boot environment redundancy, power-failure behavior and a physical recovery
procedure are proven on the matched board.

## Recovery interfaces

Normal structured control uses USB vendor bulk; CDC ACM remains available for a
minimal console even when Ethernet is absent. Recovery must provide read-only
identity, boot reason, active/pending slot, rollback index, update verification
failure, FPGA/firmware build IDs and diagnostic counters. Destructive commands
require local authorization and never implicitly enable TX.

If both custom slots fail, remove the custom SD card and restore the imaged
factory medium. Do not write QSPI or internal flash based on public artifacts:
the checked-in hardware candidate currently records unresolved contradictions
between those artifacts. JTAG recovery is permitted only after voltage, pinout,
chain identity and revision-matched vendor procedure are independently verified.
