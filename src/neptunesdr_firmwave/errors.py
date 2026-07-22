"""Domain-specific Firmwave failures."""


class FirmwaveError(Exception):
    """Base class for failures with defined firmware-tooling semantics."""


class FirmwareFormatError(FirmwaveError, ValueError):
    """A firmware artifact is malformed or fails an integrity check."""


class InterfaceError(FirmwaveError, ValueError):
    """The published firmware interface is absent or malformed."""
