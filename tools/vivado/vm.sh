#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
INSTANCE=neptune-vivado-2023-2
CONFIG="$ROOT/tools/vivado/lima-vivado-2023.2.yaml"

usage() {
  printf '%s\n' "usage: $0 {create|start|status|doctor|sync|shell|stop}"
}

command=${1:-}
case "$command" in
  create)
    if limactl list "$INSTANCE" --format '{{.Name}}' 2>/dev/null | grep -qx "$INSTANCE"; then
      printf '%s\n' "error: Lima instance already exists: $INSTANCE" >&2
      exit 2
    fi
    exec limactl start --name="$INSTANCE" --tty=false --timeout=45m "$CONFIG"
    ;;
  start)
    exec limactl start --tty=false --timeout=45m "$INSTANCE"
    ;;
  status)
    exec limactl list "$INSTANCE" --format '{{.Status}} {{.Arch}} cpus={{.CPUs}} memory={{.Memory}} disk={{.Disk}} ssh_port={{.SSHLocalPort}}'
    ;;
  doctor)
    guest_arch=$(limactl shell "$INSTANCE" uname -m)
    if [ "$guest_arch" != x86_64 ]; then
      printf '%s\n' "error: Vivado worker architecture is $guest_arch, expected x86_64" >&2
      exit 1
    fi
    limactl shell "$INSTANCE" test -f /etc/neptune-vivado-worker
    limactl shell "$INSTANCE" grep -Fx 'vivado-required=2023.2' /etc/neptune-vivado-worker
    if limactl shell "$INSTANCE" test -x /opt/amd/Vivado/2023.2/bin/vivado; then
      limactl shell "$INSTANCE" /opt/amd/Vivado/2023.2/bin/vivado -version
    else
      printf '%s\n' 'VIVADO_VM_READY suite=missing expected=/opt/amd/Vivado/2023.2' >&2
      exit 1
    fi
    ;;
  sync)
    if [ -n "$(git -C "$ROOT" status --porcelain=v1 --untracked-files=all)" ]; then
      printf '%s\n' 'error: refusing to sync a dirty source tree; commit the exact build input first' >&2
      exit 2
    fi
    commit=$(git -C "$ROOT" rev-parse --verify HEAD)
    case "$commit" in
      *[!0-9a-f]*|'') printf '%s\n' 'error: invalid source commit identity' >&2; exit 2 ;;
    esac
    if [ "${#commit}" -ne 40 ]; then
      printf '%s\n' 'error: source commit must be a full SHA-1 object ID' >&2
      exit 2
    fi
    temporary=$(mktemp -d)
    bundle="$temporary/firmwave-$commit.bundle"
    cleanup_sync() {
      if [ -f "$bundle" ]; then rm -- "$bundle"; fi
      if [ -d "$temporary" ]; then rmdir "$temporary"; fi
    }
    trap cleanup_sync EXIT HUP INT TERM
    git -C "$ROOT" bundle create "$bundle" HEAD
    bundle_sha=$(shasum -a 256 "$bundle" | awk '{print $1}')
    guest_bundle="/tmp/firmwave-$commit.bundle"
    guest_target="/opt/neptune-build/firmwave-$commit"
    if limactl shell "$INSTANCE" test -e "$guest_target"; then
      printf '%s\n' "error: immutable guest source target already exists: $guest_target" >&2
      exit 2
    fi
    limactl copy --backend=rsync "$bundle" "$INSTANCE:$guest_bundle"
    guest_uid=$(limactl shell "$INSTANCE" id -u)
    guest_gid=$(limactl shell "$INSTANCE" id -g)
    case "$guest_uid:$guest_gid" in
      *[!0-9:]*) printf '%s\n' 'error: invalid guest user identity' >&2; exit 2 ;;
    esac
    limactl shell "$INSTANCE" sudo install -d -o "$guest_uid" -g "$guest_gid" "$guest_target"
    limactl shell "$INSTANCE" git clone --quiet "$guest_bundle" "$guest_target/repository"
    limactl shell "$INSTANCE" git -C "$guest_target/repository" checkout --quiet --detach "$commit"
    observed=$(limactl shell "$INSTANCE" sha256sum "$guest_bundle" | awk '{print $1}')
    if [ "$observed" != "$bundle_sha" ]; then
      printf '%s\n' 'error: guest source-bundle SHA-256 mismatch' >&2
      exit 1
    fi
    limactl shell "$INSTANCE" rm "$guest_bundle"
    printf '%s\n' "VIVADO_VM_SOURCE commit=$commit bundle_sha256=$bundle_sha path=$guest_target/repository"
    ;;
  shell)
    exec limactl shell "$INSTANCE"
    ;;
  stop)
    exec limactl stop "$INSTANCE"
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
