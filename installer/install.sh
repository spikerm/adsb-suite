#!/usr/bin/env bash
set -euo pipefail
[ "$EUID" -eq 0 ] || { echo "Gebruik: sudo ./installer/install.sh"; exit 1; }
SRC="$(cd "$(dirname "$0")/.." && pwd)"
echo "ADS-B Suite v0.2.0 installeren/upgraden…"
apt-get update
apt-get install -y python3 python3-venv curl
id adsbsuite >/dev/null 2>&1 || useradd --system --home /opt/adsb-suite --shell /usr/sbin/nologin adsbsuite
# Let the service read /run/readsb even on installations where the directory is group restricted.
if getent group readsb >/dev/null; then usermod -a -G readsb adsbsuite; fi
systemctl disable --now adsb-homey-api.service 2>/dev/null || true
systemctl stop adsb-suite.service 2>/dev/null || true
mkdir -p /opt/adsb-suite /etc/adsb-suite /var/lib/adsb-suite
rm -rf /opt/adsb-suite/server
cp -a "$SRC/server" /opt/adsb-suite/
python3 -m venv /opt/adsb-suite/venv
/opt/adsb-suite/venv/bin/pip install --upgrade pip
/opt/adsb-suite/venv/bin/pip install -r /opt/adsb-suite/server/requirements.txt
if [ ! -f /etc/adsb-suite/config.json ]; then
  cp "$SRC/server/config.example.json" /etc/adsb-suite/config.json
else
  python3 - /etc/adsb-suite/config.json <<'PY'
import json,sys
p=sys.argv[1]
with open(p,encoding='utf-8') as f: c=json.load(f)
c.pop('source_url',None)
c.setdefault('source_file','/run/readsb/aircraft.json')
c.setdefault('aircraft_database_path','/var/lib/adsb-suite/aircraft.csv')
with open(p,'w',encoding='utf-8') as f: json.dump(c,f,indent=2); f.write('\n')
PY
fi
[ -f /var/lib/adsb-suite/aircraft.csv ] || cp "$SRC/data/aircraft.example.csv" /var/lib/adsb-suite/aircraft.csv
cp "$SRC/installer/adsb-suite.service" /etc/systemd/system/adsb-suite.service
chown -R adsbsuite:adsbsuite /opt/adsb-suite /var/lib/adsb-suite
chown root:adsbsuite /etc/adsb-suite /etc/adsb-suite/config.json
chmod 750 /etc/adsb-suite
chmod 640 /etc/adsb-suite/config.json
chmod 644 /var/lib/adsb-suite/aircraft.csv
systemctl daemon-reload
systemctl enable --now adsb-suite.service
sleep 2
systemctl --no-pager --full status adsb-suite.service || true
IP=$(hostname -I | awk '{print $1}')
echo
echo "Dashboard: http://${IP}:8090/"
echo "API:       http://${IP}:8090/api/status"
echo "Log:       sudo journalctl -u adsb-suite -f"
