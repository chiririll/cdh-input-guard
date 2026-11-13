#!/usr/bin/env bash
# Print a JSON build matrix with the newest Proxmox kernel headers per kernel
# series, taken from the community (pve-no-subscription) repository.
# Usage: discover-kernels.sh [suite...]   (default: bookworm trixie)
set -euo pipefail

suites=("$@")
[ ${#suites[@]} -gt 0 ] || suites=(bookworm trixie)   # PVE 8, PVE 9

entries=()
for suite in "${suites[@]}"; do
  url="http://download.proxmox.com/debian/pve/dists/$suite/pve-no-subscription/binary-amd64/Packages.gz"
  versions=$(curl -fsSL --retry 3 "$url" | gunzip | awk '/^Package: /{print $2}' \
    | sed -nE 's/^(proxmox|pve)-headers-([0-9]+\.[0-9]+\.[0-9]+-[0-9]+-pve)$/\2/p' \
    | sort -uV)
  for series in $(sed -E 's/^([0-9]+\.[0-9]+)\..*/\1/' <<<"$versions" | sort -uV); do
    kver=$(grep -E "^${series//./\\.}\." <<<"$versions" | tail -n1)
    entries+=("{\"suite\":\"$suite\",\"kver\":\"$kver\"}")
  done
done

[ ${#entries[@]} -gt 0 ] || { echo "no kernels found" >&2; exit 1; }
printf '%s\n' "${entries[@]}" | jq -sc '{include: .}'
