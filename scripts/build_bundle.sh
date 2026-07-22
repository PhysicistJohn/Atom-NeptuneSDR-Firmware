#!/usr/bin/env bash
# Build, fetch, validate, and assemble the complete non-flashable QEMU bundle.

set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PYTHON=${PYTHON:-python3}
CACHE_DIR=${FIRMWAVE_CACHE_DIR:-"$ROOT/.cache/firmware"}
RUNTIME_OUTPUT=${FIRMWAVE_RUNTIME_OUTPUT:-"$ROOT/.cache/p210-runtime"}
GUEST_OUTPUT=${P210_GUEST_OUTPUT:-"$ROOT/.cache/p210-guest/neptune-fft-streamer"}

export PYTHONPATH="$ROOT/src${PYTHONPATH:+:$PYTHONPATH}"
export P210_GUEST_OUTPUT="$GUEST_OUTPUT"

"$ROOT/scripts/build_guest_fft.sh"
"$PYTHON" "$ROOT/scripts/fetch_firmware.py" --cache-dir "$CACHE_DIR" --json
"$PYTHON" "$ROOT/scripts/test_firmware.py" --cache-dir "$CACHE_DIR" --json
"$PYTHON" -m neptunesdr_firmwave validate-xsa \
  --artifact p210-system-xsa \
  --cache-dir "$CACHE_DIR" \
  --json
"$PYTHON" "$ROOT/scripts/prepare_runtime.py" \
  --cache-dir "$CACHE_DIR" \
  --output "$RUNTIME_OUTPUT" \
  --iiod-exec-probe \
  --tcp-iiod \
  --tcp-probe-init \
  --fft-streamer "$GUEST_OUTPUT" \
  --json

printf 'FIRMWAVE_BUNDLE PASS\n'
printf 'runtime=%s\n' "$RUNTIME_OUTPUT"
printf 'manifest=%s\n' "$RUNTIME_OUTPUT/runtime-manifest.json"
