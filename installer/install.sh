#!/usr/bin/env bash
set -euo pipefail
[ "$EUID" -eq 0 ] || { echo "Gebruik: sudo ./installer/install.sh"; exit 1; }
SRC="$(cd "$(dirname "$0")/.." && pwd)"
echo "ADS-B Suite v0.3.0-beta2 installeren/upgraden…"
apt-get update
apt-get install -y python3 python3-venv curl gzip

if ! getent group adsbsuite >/dev/null; then groupadd --system adsbsuite; fi
if ! id adsbsuite >/dev/null 2>&1; then
  useradd --system --gid adsbsuite --home-dir /opt/adsb-suite --no-create-home --shell /usr/sbin/nologin adsbsuite
else
  usermod -g adsbsuite adsbsuite
fi
if getent group readsb >/dev/null; then usermod -a -G readsb adsbsuite; fi

systemctl disable --now adsb-homey-api.service 2>/dev/null || true
systemctl stop adsb-suite.service 2>/dev/null || true
mkdir -p /opt/adsb-suite /etc/adsb-suite /var/lib/adsb-suite /usr/local/share/tar1090

if [ -f /etc/adsb-suite/config.json ]; then
  cp -a /etc/adsb-suite/config.json "/etc/adsb-suite/config.json.bak-$(date +%Y%m%d-%H%M%S)"
fi

# Volledige tar1090/readsb vliegtuigdatabase. Hiermee voegt readsb onder andere
# registratie (r), ICAO-type (t), lange omschrijving (desc), eigenaar/operator
# (ownOp), bouwjaar en databasevlaggen toe aan aircraft.json.
DB_URL="https://github.com/wiedehopf/tar1090-db/raw/csv/aircraft.csv.gz"
DB_FILE="/usr/local/share/tar1090/aircraft.csv.gz"
DB_TMP="${DB_FILE}.new"
echo "Vliegtuigdatabase bijwerken…"
if curl -fL --retry 3 --connect-timeout 20 -o "$DB_TMP" "$DB_URL"; then
  gzip -t "$DB_TMP"
  mv -f "$DB_TMP" "$DB_FILE"
  chmod 644 "$DB_FILE"
else
  rm -f "$DB_TMP"
  [ -s "$DB_FILE" ] || { echo "Vliegtuigdatabase kon niet worden gedownload."; exit 1; }
  echo "Download mislukt; bestaande vliegtuigdatabase blijft in gebruik."
fi

# Zorg dat readsb de database werkelijk gebruikt. De standaard Debian/readsb
# service leest DECODER_OPTIONS uit /etc/default/readsb.
if [ -f /etc/default/readsb ]; then
  cp -a /etc/default/readsb "/etc/default/readsb.bak-adsbsuite-$(date +%Y%m%d-%H%M%S)"
  python3 - /etc/default/readsb "$DB_FILE" <<'PY'
from pathlib import Path
import re, shlex, sys
p = Path(sys.argv[1])
db = sys.argv[2]
s = p.read_text(encoding='utf-8')
m = re.search(r'(?m)^DECODER_OPTIONS=(.*)$', s)
raw = m.group(1).strip() if m else '""'
try:
    value = shlex.split(raw)[0] if raw else ''
except ValueError:
    value = raw.strip('"\'')
args = shlex.split(value)
clean = []
skip = False
for arg in args:
    if arg.startswith('--db-file=') or arg == '--db-file' or arg == '--db-file-lt':
        skip = arg == '--db-file'
        continue
    if skip:
        skip = False
        continue
    clean.append(arg)
clean += [f'--db-file={db}', '--db-file-lt']
quoted = '"' + ' '.join(clean).replace('\\', '\\\\').replace('"', '\\"') + '"'
line = f'DECODER_OPTIONS={quoted}'
if m:
    s = s[:m.start()] + line + s[m.end():]
else:
    s += ('\n' if s and not s.endswith('\n') else '') + line + '\n'
p.write_text(s, encoding='utf-8')
PY
  systemctl restart readsb.service 2>/dev/null || systemctl restart readsb 2>/dev/null || true
  sleep 3
else
  echo "Waarschuwing: /etc/default/readsb ontbreekt; database is wel gedownload maar niet aan readsb gekoppeld."
fi

rm -rf /opt/adsb-suite/server
cp -a "$SRC/server" /opt/adsb-suite/

# readsb gebruikt `type` voor de databron (zoals adsb_icao), niet als ICAO-type.
# Gebruik daarnaast de metadata die --db-file-lt aan aircraft.json toevoegt.
python3 - /opt/adsb-suite/server/adsb_suite.py <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text(encoding='utf-8')
s = s.replace(', metadata.get("icao_type"), item.get("type"))', ', metadata.get("icao_type"))')
s = s.replace('"description": first_text(metadata.get("description"), model),', '"description": first_text(item.get("desc"), metadata.get("description"), model),')
s = s.replace('"operator": first_text(metadata.get("operator"), metadata.get("owner"), metadata.get("airline")),', '"operator": first_text(item.get("ownOp"), metadata.get("operator"), metadata.get("owner"), metadata.get("airline")),')
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
c['receiver_lat']=51.842837320295985
c['receiver_lon']=4.69044839675828
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
sleep 3
systemctl --no-pager --full status adsb-suite.service || true
IP=$(hostname -I | awk '{print $1}')
echo
echo "Dashboard: http://${IP}:8090/"
echo "API:       http://${IP}:8090/api/status"
echo "Tracks:    http://${IP}:8090/api/track/<hex>?minutes=30"
echo "Database:  $DB_FILE"
echo "Log:       sudo journalctl -u adsb-suite -f"