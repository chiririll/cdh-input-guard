#!/usr/bin/env bash
# Build the module for one kernel and copy it to out/.
# Usage: build.sh <kver>
set -euo pipefail
kver=$1

make KVER="$kver" KDIR="/usr/src/linux-headers-$kver"

# Kernels before 6.13 ignore MO= and leave the .ko in src/.
ko=$(find build src -name cdh_input_guard.ko | head -n1)
[ -n "$ko" ] || { echo "cdh_input_guard.ko not found" >&2; exit 1; }
mkdir -p out
cp "$ko" "out/cdh_input_guard-$kver.ko"
modinfo "out/cdh_input_guard-$kver.ko"
