"""Domain-specific Firmware failures."""


class FirmwareError(Exception):
    """Base class for failures with defined firmware-tooling semantics."""


class FirmwareFormatError(FirmwareError, ValueError):
    """A firmware artifact is malformed or fails an integrity check."""


class InterfaceError(FirmwareError, ValueError):
    """The published firmware interface is absent or malformed."""


class ProvenanceError(FirmwareError, ValueError):
    """A requested source identity cannot be established from Git."""
