#!/usr/bin/env bash
set -u
fail=0
ok(){ printf '✔ %s\n' "$1"; }
bad(){ printf '✖ %s\n' "$1"; fail=1; }

command -v python3 >/dev/null && ok "Python 3 aanwezig" || bad "Python 3 ontbreekt"
command -v readsb >/dev/null && ok "readsb aanwezig" || bad "readsb niet gevonden"
[ -r /run/readsb/aircraft.json ] && ok "aircraft.json leesbaar" || bad "/run/readsb/aircraft.json niet leesbaar"
systemctl is-active --quiet adsb-suite.service && ok "adsb-suite.service actief" || bad "adsb-suite.service niet actief"
[ -f /etc/adsb-suite/config.json ] && ok "Configuratie aanwezig" || bad "Configuratie ontbreekt"
[ -f /usr/local/share/tar1090/aircraft.csv.gz ] && ok "Vliegtuigdatabase aanwezig" || bad "Vliegtuigdatabase ontbreekt"

if command -v curl >/dev/null && curl -fsS --max-time 5 http://127.0.0.1:8090/api/status >/tmp/adsb-suite-status.json; then
  ok "API bereikbaar op poort 8090"
  cat /tmp/adsb-suite-status.json
  echo
else
  bad "API niet bereikbaar op poort 8090"
fi

if [ "$fail" -ne 0 ]; then
  echo "Log: sudo journalctl -u adsb-suite -n 100 --no-pager"
fi
exit "$fail"
