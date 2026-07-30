# ADS-B Suite v0.7

Lokale ADS-B backend en webdashboard voor `readsb`, met live radar, vluchtinformatie, historische tracks, statistieken, analyse, filters en waarschuwingen.

## Installeren met één commando

Op Raspberry Pi OS of Debian/Ubuntu:

```bash
curl -fsSL https://raw.githubusercontent.com/spikerm/adsb-suite/main/install.sh | sudo bash
```

Alternatief met `wget`:

```bash
wget -qO- https://raw.githubusercontent.com/spikerm/adsb-suite/main/install.sh | sudo bash
```

De installer controleert de benodigde pakketten, downloadt de repository naar `/opt/adsb-suite-src`, voert de interne installer uit en installeert beheercommando's.

Open daarna:

```text
http://<IP-VAN-DE-PI>:8090/
```

## Testversie of andere branch installeren

```bash
curl -fsSL https://raw.githubusercontent.com/spikerm/adsb-suite/feature/v0.7/install.sh \
  | sudo ADSB_SUITE_REF=feature/v0.7 bash
```

## Beheer

```bash
sudo adsb-suite-update
sudo adsb-suite-doctor
sudo adsb-suite-uninstall
```

`adsb-suite-uninstall` behoudt standaard de configuratie en historische gegevens in `/etc/adsb-suite` en `/var/lib/adsb-suite`.

## Handmatig installeren

```bash
git clone https://github.com/spikerm/adsb-suite.git
cd adsb-suite
sudo ./installer/install.sh
```

## Vereisten

- Raspberry Pi OS, Debian of Ubuntu
- `readsb`
- Leesbare live bron: `/run/readsb/aircraft.json`
- Netwerktoegang voor kaarttegels, vliegtuigdatabase, route- en fotoinformatie

## Belangrijkste functies

- Live radar en vliegtuiglijst
- Registratie, type, model, operator en route
- Vliegtuigfoto's met fallback
- Tracks van 5, 30 en 60 minuten met playback
- Hoogte- en snelheidsprofielen
- OpenStreetMap, topografische en satellietkaart
- Ontvangstanalyse en polarplot
- Filters voor heavies, helikopters, militair verkeer en noodsituaties
- Detectie van squawk 7500, 7600 en 7700
- Browsermeldingen en lokale waarschuwingen
- SQLite-historie, statistieken en CSV-export
- Raspberry Pi-systeemstatus

## Belangrijkste endpoints

- `/api/status`
- `/api/aircraft`
- `/api/nearest`
- `/api/track/{hex}?minutes=30`
- `/api/enrichment/{hex}?flight=KLM123`
- `/api/history?hours=24&hex=484abc`
- `/api/history/stats`
- `/api/search?q=KLM`
- `/api/export.csv?hours=24`
- `/live`
- `/health`

## Problemen controleren

```bash
sudo adsb-suite-doctor
sudo journalctl -u adsb-suite -n 100 --no-pager
```

De doctor controleert onder andere Python, readsb, `aircraft.json`, de systemd-service, configuratie, vliegtuigdatabase en de lokale API.
