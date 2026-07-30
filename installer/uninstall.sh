#!/usr/bin/env bash
set -euo pipefail
[ "$EUID" -eq 0 ] || { echo "Gebruik sudo"; exit 1; }
systemctl disable --now adsb-suite.service 2>/dev/null || true
rm -f /etc/systemd/system/adsb-suite.service
rm -rf /opt/adsb-suite
systemctl daemon-reload
echo "Software verwijderd. Config/database behouden in /etc/adsb-suite en /var/lib/adsb-suite."
