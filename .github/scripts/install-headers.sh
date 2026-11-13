#!/usr/bin/env bash
# Install build tools and Proxmox kernel headers inside a Debian container.
# Usage: install-headers.sh <suite> <kver>
set -euo pipefail
suite=$1
kver=$2

apt-get update
apt-get install -y --no-install-recommends build-essential kmod bc libelf-dev ca-certificates curl gnupg

curl -fsSL "https://enterprise.proxmox.com/debian/proxmox-release-$suite.gpg" \
  -o "/etc/apt/trusted.gpg.d/proxmox-release-$suite.gpg"
echo "deb http://download.proxmox.com/debian/pve $suite pve-no-subscription" \
  > /etc/apt/sources.list.d/pve.list
apt-get update

pkg=$(apt-cache search --names-only "^(proxmox|pve)-headers-${kver}\$" | awk '{print $1}' | head -n1)
[ -n "$pkg" ] || { echo "no headers package for $kver" >&2; exit 1; }
apt-get install -y --no-install-recommends "$pkg"
test -d "/usr/src/linux-headers-$kver"
