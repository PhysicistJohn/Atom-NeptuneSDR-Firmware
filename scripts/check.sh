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
sh -n scripts/build_guest_fft.sh

python3 -m neptunesdr_firmware validate-locks --json >/dev/null
python3 -m neptunesdr_firmware interface --json >/dev/null
python3 -m neptunesdr_firmware source-identity --json >/dev/null
python3 scripts/fetch_firmware.py --help >/dev/null
python3 scripts/test_firmware.py --help >/dev/null
python3 scripts/prepare_runtime.py --help >/dev/null
python3 scripts/qemu_boot.py --help >/dev/null
git diff --check

printf 'NEPTUNE_FIRMWARE_SOURCE_GATE PASS\n'
