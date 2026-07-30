#!/usr/bin/env bash
set -euo pipefail
[ "${EUID}" -eq 0 ] || { echo "Gebruik: sudo adsb-suite-uninstall"; exit 1; }

systemctl disable --now adsb-suite.service 2>/dev/null || true
rm -f /etc/systemd/system/adsb-suite.service
systemctl daemon-reload
rm -rf /opt/adsb-suite
rm -f /usr/local/sbin/adsb-suite-update /usr/local/sbin/adsb-suite-doctor /usr/local/sbin/adsb-suite-uninstall

echo "ADS-B Suite is verwijderd."
echo "De configuratie en database in /etc/adsb-suite en /var/lib/adsb-suite zijn behouden."
echo "Verwijder die mappen handmatig wanneer ook de gegevens weg mogen."
