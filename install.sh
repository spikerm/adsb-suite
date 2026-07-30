#!/usr/bin/env bash
set -euo pipefail

REPO_URL="${ADSB_SUITE_REPO:-https://github.com/spikerm/adsb-suite.git}"
REF="${ADSB_SUITE_REF:-main}"
SRC_DIR="${ADSB_SUITE_SRC_DIR:-/opt/adsb-suite-src}"

if [ "${EUID}" -ne 0 ]; then
  echo "Start dit script met sudo of als root."
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y git ca-certificates curl

if [ -d "$SRC_DIR/.git" ]; then
  git -C "$SRC_DIR" fetch --prune origin
else
  rm -rf "$SRC_DIR"
  git clone "$REPO_URL" "$SRC_DIR"
fi

git -C "$SRC_DIR" checkout -f "$REF"
git -C "$SRC_DIR" reset --hard "origin/$REF" 2>/dev/null || true
chmod +x "$SRC_DIR/installer/install.sh"
"$SRC_DIR/installer/install.sh"

install -m 755 "$SRC_DIR/update.sh" /usr/local/sbin/adsb-suite-update 2>/dev/null || true
install -m 755 "$SRC_DIR/doctor.sh" /usr/local/sbin/adsb-suite-doctor 2>/dev/null || true
install -m 755 "$SRC_DIR/uninstall.sh" /usr/local/sbin/adsb-suite-uninstall 2>/dev/null || true

echo
echo "ADS-B Suite is geïnstalleerd."
echo "Update:      sudo adsb-suite-update"
echo "Controle:    sudo adsb-suite-doctor"
echo "Verwijderen: sudo adsb-suite-uninstall"
