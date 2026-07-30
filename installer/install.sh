#!/usr/bin/env bash
set -euo pipefail
[ "$EUID" -eq 0 ] || { echo "Gebruik: sudo ./installer/install.sh"; exit 1; }
SRC="$(cd "$(dirname "$0")/.." && pwd)"
echo "ADS-B Suite v0.3.0-beta2 installeren/upgraden…"
apt-get update
apt-get install -y python3 python3-venv curl
if ! getent group adsbsuite >/dev/null; then groupadd --system adsbsuite; fi
if ! id adsbsuite >/dev/null 2>&1; then
  useradd --system --gid adsbsuite --home-dir /opt/adsb-suite --no-create-home --shell /usr/sbin/nologin adsbsuite
else
  usermod -g adsbsuite adsbsuite
fi
if getent group readsb >/dev/null; then usermod -a -G readsb adsbsuite; fi
systemctl disable --now adsb-homey-api.service 2>/dev/null || true
systemctl stop adsb-suite.service 2>/dev/null || true
mkdir -p /opt/adsb-suite /etc/adsb-suite /var/lib/adsb-suite
if [ -f /etc/adsb-suite/config.json ]; then
  cp -a /etc/adsb-suite/config.json "/etc/adsb-suite/config.json.bak-$(date +%Y%m%d-%H%M%S)"
fi
rm -rf /opt/adsb-suite/server
cp -a "$SRC/server" /opt/adsb-suite/
# readsb gebruikt `type` voor de bron van het bericht (bijv. adsb_icao), niet voor het vliegtuigtype.
# Verwijder daarom deze onjuiste fallback; ICAO-type komt uit `t` of de aircraft-database.
python3 - /opt/adsb-suite/server/adsb_suite.py <<'PY'
from pathlib import Path
p = Path(__import__('sys').argv[1])
s = p.read_text(encoding='utf-8')
s = s.replace(', metadata.get("icao_type"), item.get("type"))', ', metadata.get("icao_type"))')
p.write_text(s, encoding='utf-8')
PY
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
c.setdefault('receiver_name','Papendrecht ADS-B')
c.setdefault('aircraft_database_path','/var/lib/adsb-suite/aircraft.csv')
c.setdefault('track_default_minutes',30)
c.setdefault('track_max_hours',24)
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
systemctl reset-failed adsb-suite.service 2>/dev/null || true
systemctl enable --now adsb-suite.service
sleep 2
systemctl --no-pager --full status adsb-suite.service || true
IP=$(hostname -I | awk '{print $1}')
echo
echo "Dashboard: http://${IP}:8090/"
echo "API:       http://${IP}:8090/api/status"
echo "Tracks:    http://${IP}:8090/api/track/<hex>?minutes=30"
echo "Log:       sudo journalctl -u adsb-suite -f"
