#!/usr/bin/env bash
# Run inside the x86-64 worker after the account owner has downloaded the AMD
# installer, reviewed a generated configuration, and accepted both licenses.
set -euo pipefail

if [ "$(uname -m)" != x86_64 ]; then
  printf '%s\n' 'error: Vivado 2023.2 worker must be x86_64' >&2
  exit 2
fi
if [ "${AMD_EULA_ACCEPTED:-}" != 'XilinxEULA,3rdPartyEULA' ]; then
  printf '%s\n' 'error: set AMD_EULA_ACCEPTED=XilinxEULA,3rdPartyEULA only after reviewing and accepting those terms' >&2
  exit 2
fi
if [ "$#" -ne 3 ]; then
  printf '%s\n' "usage: AMD_EULA_ACCEPTED=XilinxEULA,3rdPartyEULA $0 INSTALLER SHA256 INSTALL_CONFIG" >&2
  exit 2
fi

installer=$1
expected_sha256=$2
install_config=$3
extract_dir=/opt/neptune-tool-cache/vivado-2023.2-installer

case "$expected_sha256" in
  *[!0-9a-f]*|'') printf '%s\n' 'error: SHA256 must be lowercase hexadecimal' >&2; exit 2 ;;
esac
if [ "${#expected_sha256}" -ne 64 ] || [ ! -f "$installer" ] || [ -L "$installer" ]; then
  printf '%s\n' 'error: installer must be a regular file with a 64-hex digest' >&2
  exit 2
fi
if [ ! -f "$install_config" ] || [ -L "$install_config" ]; then
  printf '%s\n' 'error: install configuration must be a reviewed regular file' >&2
  exit 2
fi
observed_sha256=$(sha256sum "$installer" | awk '{print $1}')
if [ "$observed_sha256" != "$expected_sha256" ]; then
  printf '%s\n' 'error: installer SHA-256 mismatch' >&2
  exit 2
fi
if [ -e "$extract_dir" ]; then
  printf '%s\n' "error: extraction target already exists: $extract_dir" >&2
  exit 2
fi

sudo install -d -m 0755 "$extract_dir"
sudo chown "$(id -u):$(id -g)" "$extract_dir"
chmod u+x "$installer"
"$installer" --keep --noexec --target "$extract_dir"
if [ ! -x "$extract_dir/xsetup" ]; then
  printf '%s\n' 'error: installer did not yield xsetup' >&2
  exit 2
fi
sudo "$extract_dir/xsetup" \
  --agree XilinxEULA,3rdPartyEULA \
  --batch Install \
  --config "$install_config"

if [ ! -x /opt/amd/Vivado/2023.2/bin/vivado ]; then
  printf '%s\n' 'error: expected Vivado executable was not installed' >&2
  exit 2
fi
/opt/amd/Vivado/2023.2/bin/vivado -version | grep -E 'Vivado v2023\.2([^0-9]|$)'
