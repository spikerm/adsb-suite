# ADS-B Suite v0.2.0

Complete lokale ADS-B backend voor `readsb` met live dashboard, radar, SQLite-historie, REST API en WebSocket.

## Nieuw in v0.2

- Leest rechtstreeks `/run/readsb/aircraft.json`; geen HTTP-lus meer.
- Live OpenStreetMap-radar met koersgedraaide vliegtuigiconen.
- Vliegtuiglijst, detailpaneel, zoekfilter en 24-uursstatistieken.
- SQLite-tabellen voor observaties en samenvatting per vliegtuig.
- Zoek-API, track-API en CSV-export.
- Veilige upgrade vanaf v0.1, inclusief database-migratie.
- Installer schakelt de oude `adsb-homey-api.service` uit om poortconflicten te voorkomen.

## Installeren/upgraden

```bash
cd adsb-suite-v0.2.0
sudo ./installer/install.sh
```

Open daarna `http://<IP-VAN-DE-PI>:8090/`.

## Belangrijkste endpoints

- `/api/status`
- `/api/aircraft`
- `/api/nearest`
- `/api/history?hours=24&hex=484abc`
- `/api/history/stats`
- `/api/search?q=KLM`
- `/api/aircraft/{hex}`
- `/api/export.csv?hours=24`
- `/live` (WebSocket)
- `/health`

## Optionele vliegtuigdatabase

Vul `/var/lib/adsb-suite/aircraft.csv` met kolommen:

```csv
hex,registration,type,description,operator
```

Herstart daarna de service.
