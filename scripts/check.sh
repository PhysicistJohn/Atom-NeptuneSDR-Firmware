#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export PYTHONPATH="$ROOT/src${PYTHONPATH:+:$PYTHONPATH}"

cd "$ROOT"
python3 -m compileall -q src scripts tests
command -v cc >/dev/null
cc -std=c11 -fsyntax-only -Wall -Wextra -Werror firmware/neptune_fft_streamer.c
python3 -m unittest discover -s tests -p 'test_*.py' -v

for script in scripts/*.sh; do
  bash -n "$script"
done
for script in tools/vivado/*.sh; do
  bash -n "$script"
done
sh -n scripts/build_guest_fft.sh

python3 -m neptunesdr_firmwave validate-locks --json >/dev/null
python3 -m neptunesdr_firmwave interface --json >/dev/null
python3 -m neptunesdr_firmwave source-identity --json >/dev/null
python3 scripts/fetch_firmware.py --help >/dev/null
python3 scripts/test_firmware.py --help >/dev/null
python3 scripts/prepare_runtime.py --help >/dev/null
python3 scripts/qemu_boot.py --help >/dev/null
python3 scripts/generate_protocol.py --check
python3 scripts/generate_registers.py --check
python3 scripts/generate_edge_profile.py --check
python3 hdl/scripts/check_hdl.py
python3 ps/kernel/neptune_stream/tests/test_neptune_stream.py
make -C ps/control-daemon clean test
make -C ps/data-service clean test
make -C tools/host clean test
python3 scripts/board_doctor.py --validate-only --json >/dev/null
python3 scripts/build_board.py --plan --json >/dev/null
git diff --check

printf 'NEPTUNE_FIRMWAVE_SOURCE_GATE PASS\n'
