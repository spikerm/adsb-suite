#!/usr/bin/env bash
set -euo pipefail
[ "$EUID" -eq 0 ] || { echo "Gebruik: sudo ./installer/install.sh"; exit 1; }
SRC="$(cd "$(dirname "$0")/.." && pwd)"
echo "ADS-B Suite v1.1.0 installeren/upgraden…"
apt-get update
apt-get install -y python3 python3-venv curl gzip sudo nginx openssl

if ! getent group adsbsuite >/dev/null; then groupadd --system adsbsuite; fi
if ! id adsbsuite >/dev/null 2>&1; then
  useradd --system --gid adsbsuite --home-dir /opt/adsb-suite --no-create-home --shell /usr/sbin/nologin adsbsuite
else
  usermod -g adsbsuite adsbsuite
fi
if getent group readsb >/dev/null; then usermod -a -G readsb adsbsuite; fi

systemctl disable --now adsb-homey-api.service 2>/dev/null || true
systemctl stop adsb-suite.service 2>/dev/null || true
mkdir -p /opt/adsb-suite /etc/adsb-suite /var/lib/adsb-suite/backups /var/lib/adsb-suite/aviation /usr/local/share/tar1090

if [ -f /etc/adsb-suite/config.json ]; then cp -a /etc/adsb-suite/config.json "/etc/adsb-suite/config.json.bak-$(date +%Y%m%d-%H%M%S)"; fi
DB_URL="https://github.com/wiedehopf/tar1090-db/raw/csv/aircraft.csv.gz"; DB_FILE="/usr/local/share/tar1090/aircraft.csv.gz"; DB_TMP="${DB_FILE}.new"
echo "Vliegtuigdatabase bijwerken…"
if curl -fL --retry 3 --connect-timeout 20 -o "$DB_TMP" "$DB_URL"; then gzip -t "$DB_TMP"; mv -f "$DB_TMP" "$DB_FILE"; chmod 644 "$DB_FILE"; else rm -f "$DB_TMP"; [ -s "$DB_FILE" ] || exit 1; fi

if [ -f /etc/default/readsb ]; then
  cp -a /etc/default/readsb "/etc/default/readsb.bak-adsbsuite-$(date +%Y%m%d-%H%M%S)"
  python3 - /etc/default/readsb "$DB_FILE" <<'PY'
from pathlib import Path
import re, shlex, sys
p=Path(sys.argv[1]); db=sys.argv[2]; s=p.read_text(encoding='utf-8')
m=re.search(r'(?m)^DECODER_OPTIONS=(.*)$',s); raw=m.group(1).strip() if m else '""'
try: value=shlex.split(raw)[0] if raw else ''
except ValueError: value=raw.strip('"\'')
args=shlex.split(value); clean=[]; skip=False
for arg in args:
    if arg.startswith('--db-file=') or arg=='--db-file' or arg=='--db-file-lt': skip=arg=='--db-file'; continue
    if skip: skip=False; continue
    clean.append(arg)
clean += [f'--db-file={db}','--db-file-lt']
line='DECODER_OPTIONS="'+' '.join(clean).replace('\\','\\\\').replace('"','\\"')+'"'
s=s[:m.start()]+line+s[m.end():] if m else s+('\n' if s and not s.endswith('\n') else '')+line+'\n'
p.write_text(s,encoding='utf-8')
PY
  systemctl restart readsb.service 2>/dev/null || systemctl restart readsb 2>/dev/null || true
fi

rm -rf /opt/adsb-suite/server; cp -a "$SRC/server" /opt/adsb-suite/
python3 - /opt/adsb-suite/server/adsb_suite.py <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text(encoding='utf-8')
for old in ('"0.3.0-beta2"','"0.4.0"','"0.5.0"','"0.6.0"','"0.7.0"','"0.8.0"','"0.8.1"','"0.9.0"','"0.9.1"','"1.0.0"'): s=s.replace(old,'"1.1.0"')
s=s.replace(', metadata.get("icao_type"), item.get("type"))', ', metadata.get("icao_type"))')
s=s.replace('"description": first_text(metadata.get("description"), model),','"description": first_text(item.get("desc"), metadata.get("description"), model),')
s=s.replace('"operator": first_text(metadata.get("operator"), metadata.get("owner"), metadata.get("airline")),','"operator": first_text(item.get("ownOp"), metadata.get("operator"), metadata.get("owner"), metadata.get("airline")),')
imports=(
 ('from v04_enrichment import enrich_aircraft','from v04_enrichment import enrich_aircraft'),
 ('from v08_admin import register_admin','from v08_admin import register_admin'),
 ('from v09_update import register_update_api','from v09_update import register_update_api'),
 ('from v10_replay import register_replay','from v10_replay import register_replay'),
 ('from v11_aviation import register_aviation_api','from v11_aviation import register_aviation_api'),
)
for needle,line in imports:
    if needle not in s: s=s.replace('from aiohttp import WSMsgType, web','from aiohttp import WSMsgType, web\n'+line)
handler='''\n\nasync def api_enrichment(request: web.Request) -> web.Response:\n    hx = request.match_info["hex"].strip().lower()\n    aircraft = next((a for a in state["aircraft"] if a.get("hex") == hx), None)\n    if aircraft is None:\n        with dbconn() as conn:\n            row = conn.execute("SELECT * FROM seen_aircraft WHERE hex=?", (hx,)).fetchone()\n        aircraft = dict(row) if row else {"hex": hx, "flight": request.query.get("flight")}\n    elif request.query.get("flight"):\n        aircraft = {**aircraft, "flight": request.query.get("flight")}\n    return web.json_response(await enrich_aircraft(aircraft), headers={"Cache-Control": "no-store"})\n'''
if 'async def api_enrichment' not in s: s=s.replace('\n\nasync def api_status',handler+'\n\nasync def api_status')
if '/api/enrichment/{hex}' not in s: s=s.replace('app.router.add_get("/api/track/{hex}", api_track)','app.router.add_get("/api/track/{hex}", api_track)\napp.router.add_get("/api/enrichment/{hex}", api_enrichment)')
registrations=(
 ('register_admin(app','register_admin(app, BASE, CFG, CONFIG_PATH, DB, state, VERSION)'),
 ('register_update_api(app','register_update_api(app, VERSION, CFG)'),
 ('register_replay(app','register_replay(app, DB)'),
 ('register_aviation_api(app','register_aviation_api(app, BASE, CFG)'),
)
for needle,line in registrations:
    if needle not in s: s=s.replace('\nif __name__ == "__main__":','\n'+line+'\n\nif __name__ == "__main__":')
p.write_text(s,encoding='utf-8')
PY

python3 -m venv /opt/adsb-suite/venv
/opt/adsb-suite/venv/bin/pip install --upgrade pip
/opt/adsb-suite/venv/bin/pip install -r /opt/adsb-suite/server/requirements.txt

if [ ! -f /etc/adsb-suite/config.json ]; then cp "$SRC/server/config.example.json" /etc/adsb-suite/config.json; fi
python3 - /etc/adsb-suite/config.json <<'PY'
import hashlib,json,secrets,sys
p=sys.argv[1]
with open(p,encoding='utf-8') as f:c=json.load(f)
c.pop('source_url',None); c.setdefault('source_file','/run/readsb/aircraft.json'); c.setdefault('receiver_name','Papendrecht ADS-B')
c.setdefault('receiver_lat',51.842837320295985); c.setdefault('receiver_lon',4.69044839675828); c.setdefault('antenna_height_m',10.0)
c.setdefault('aircraft_database_path','/var/lib/adsb-suite/aircraft.csv'); c.setdefault('track_default_minutes',30); c.setdefault('track_max_hours',24)
c.setdefault('update_channel','feature/v0.9-smart-dashboard'); c.setdefault('aviation_data_dir','/var/lib/adsb-suite/aviation')
if not c.get('admin_password_hash'):
    password=secrets.token_urlsafe(12); salt=secrets.token_hex(16); digest=hashlib.pbkdf2_hmac('sha256',password.encode(),salt.encode(),240000).hex()
    c['admin_password_hash']=f'pbkdf2_sha256${salt}${digest}'; open('/run/adsb-suite-admin-password','w').write(password)
with open(p,'w',encoding='utf-8') as f:json.dump(c,f,indent=2,ensure_ascii=False);f.write('\n')
PY

for layer in navaids waypoints ctr tma fir restricted danger prohibited; do
  [ -f "/var/lib/adsb-suite/aviation/${layer}.geojson" ] || printf '%s\n' '{"type":"FeatureCollection","features":[]}' > "/var/lib/adsb-suite/aviation/${layer}.geojson"
done
[ -f /var/lib/adsb-suite/aircraft.csv ] || cp "$SRC/data/aircraft.example.csv" /var/lib/adsb-suite/aircraft.csv
cp "$SRC/installer/adsb-suite.service" /etc/systemd/system/adsb-suite.service
install -m 755 "$SRC/installer/adsb-suite-admin-helper" /usr/local/sbin/adsb-suite-admin-helper
install -m 440 "$SRC/installer/adsb-suite-admin.sudoers" /etc/sudoers.d/adsb-suite-admin
visudo -cf /etc/sudoers.d/adsb-suite-admin >/dev/null
chown -R adsbsuite:adsbsuite /opt/adsb-suite /var/lib/adsb-suite
chown root:adsbsuite /etc/adsb-suite /etc/adsb-suite/config.json
chmod 750 /etc/adsb-suite; chmod 660 /etc/adsb-suite/config.json; chmod 644 /var/lib/adsb-suite/aircraft.csv /var/lib/adsb-suite/aviation/*.geojson
systemctl daemon-reload; systemctl reset-failed adsb-suite.service 2>/dev/null || true; systemctl enable --now adsb-suite.service
chmod +x "$SRC/installer/setup-https.sh"; "$SRC/installer/setup-https.sh"
sleep 3
IP=$(hostname -I | awk '{print $1}')
echo; echo "ADS-B Suite v1.1.0 geïnstalleerd"; echo "Dashboard HTTPS: https://${IP}:8443/"; echo "Setup wizard:    https://${IP}:8443/setup"; echo "Webbeheer:       https://${IP}:8443/admin"
if [ -s /run/adsb-suite-admin-password ]; then echo "Beheerwachtwoord: $(cat /run/adsb-suite-admin-password)"; rm -f /run/adsb-suite-admin-password; fi
