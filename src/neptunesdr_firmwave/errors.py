"""Domain-specific Firmwave failures."""


class FirmwaveError(Exception):
    """Base class for failures with defined firmware-tooling semantics."""


class FirmwareFormatError(FirmwaveError, ValueError):
    """A firmware artifact is malformed or fails an integrity check."""


class InterfaceError(FirmwaveError, ValueError):
    """The published firmware interface is absent or malformed."""


class ProvenanceError(FirmwaveError, ValueError):
    """A requested source identity cannot be established from Git."""


class BoardBuildError(FirmwaveError, ValueError):
    """A physical-board build definition or execution failed closed."""


class HardwareManifestError(FirmwaveError, ValueError):
    """A hardware-evidence manifest is missing or structurally invalid."""
