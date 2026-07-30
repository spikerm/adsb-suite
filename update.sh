#!/usr/bin/env bash
set -euo pipefail
SRC_DIR="${ADSB_SUITE_SRC_DIR:-/opt/adsb-suite-src}"
REF="${ADSB_SUITE_REF:-main}"
[ "${EUID}" -eq 0 ] || { echo "Gebruik: sudo adsb-suite-update"; exit 1; }
[ -d "$SRC_DIR/.git" ] || { echo "Bronmap ontbreekt: $SRC_DIR"; exit 1; }
git -C "$SRC_DIR" fetch --prune origin
git -C "$SRC_DIR" checkout -f "$REF"
git -C "$SRC_DIR" reset --hard "origin/$REF"
chmod +x "$SRC_DIR/installer/install.sh"
"$SRC_DIR/installer/install.sh"
echo "ADS-B Suite is bijgewerkt vanaf $REF."
