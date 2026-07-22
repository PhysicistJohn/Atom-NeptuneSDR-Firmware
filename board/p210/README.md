# P210 physical-board scaffold

This directory defines profile `p210-sd-development-v0`. It is a fail-closed
build contract, not a bootable release. Every component shares compatibility ID
`p210-xc7z020-custom-v0`, but unresolved sources and stage drivers intentionally
block execution until revision-matched material is selected and locked.

The only permitted installation medium is removable SD. This repository does
not include a host block-device writer, QSPI update command, or recovery claim.
TX is disabled at boot by policy; that declaration must later be verified in
RTL, the kernel driver, the control daemon, and hardware tests.

Run the structural check without requiring proprietary tools:

```sh
python3 scripts/board_doctor.py --validate-only
```

Run the environment doctor to see every unresolved source, missing exact tool,
and blocked stage:

```sh
python3 scripts/board_doctor.py
```

`scripts/build_board.py` refuses to execute until the source lock, toolchain
lock, and every direct-argv stage are resolved. See `docs/BOARD_BUILD.md` for
the resolution and manifest contract.
