# Firmware provenance and evidence boundary

Firmware produces a development runtime from three immutable public inputs.
Their URLs, exact byte counts, SHA-256 digests, kinds, and evidence levels live
in `src/neptunesdr_firmware/data/firmware-lock.json`:

- a public P210 SD boot partition and XSA from the community
  `Neptune-SDR-nix-utils` commit pinned by the URLs;
- the official Analog Devices PlutoSDR firmware v0.39 release ZIP.

`runtime-lock.json` records the audited kernel, device-tree, rootfs, ARM ABI,
iiod, and libiio identities used by the composition. Downloading is
content-addressed and atomic. Validation rejects size or digest changes,
malformed tar/ZIP/FIT/uImage/DTB/DFU/CPIO/ELF structures, unsafe archive paths,
and incompatible P210 XSA contacts.

The composition is useful because the public P210 kernel/device tree and the
official Pluto rootfs are ARM ABI compatible. It is not an image published by
HAMGEEK and it is not marked flashable. The public P210 device tree also
conflicts with the Pluto gadget rootfs at the USB-controller role, and the
pinned community recipe reports that AD9361 was non-functional. Those facts
are retained as manifest limitations instead of being papered over.

The runtime manifest captures the exact Firmware commit/tree/source state,
canonical interface hash, input hashes, and every published runtime artifact
hash. This makes a digital-twin execution reproducible; it does not establish
physical RF, timing closure, DDR coherency, USB electrical behavior, or safe
flash/recovery behavior. Those require the arriving board and vendor material.
