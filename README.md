# ADS-B Suite v0.8

Lokale ADS-B backend en webdashboard voor `readsb`, met live radar, vluchtinformatie, historische tracks, statistieken, analyse, filters, waarschuwingen en een beveiligde webbeheeromgeving.

## Installeren met één commando

Op Raspberry Pi OS of Debian/Ubuntu:

```bash
curl -fsSL https://raw.githubusercontent.com/spikerm/adsb-suite/main/install.sh | sudo bash
```

Testversie V0.8 installeren:

```bash
curl -fsSL https://raw.githubusercontent.com/spikerm/adsb-suite/feature/v0.8-webadmin/install.sh \
  | sudo ADSB_SUITE_REF=feature/v0.8-webadmin bash
```

De installer toont bij een nieuwe installatie één keer een willekeurig beheerderswachtwoord. Bewaar dit wachtwoord.

Open daarna:

```text
Radar:      http://<IP-VAN-DE-PI>:8090/
Webbeheer:  http://<IP-VAN-DE-PI>:8090/admin
```

## Webbeheer V0.8

De beheeromgeving bevat:

- login met PBKDF2-wachtwoordhash en beveiligde sessiecookie;
- status van ADS-B Suite, readsb en tar1090;
- live vliegtuigen, berichten per seconde, CPU-temperatuur, load, schijf- en databasegebruik;
- receivernaam en antennecoördinaten wijzigen;
- polling, historie, bewaartermijn en trackinstellingen wijzigen;
- ADS-B Suite en readsb herstarten;
- Raspberry Pi herstarten met bevestiging;
- vliegtuigdatabase bijwerken;
- software-update starten;
- logboeken van ADS-B Suite, readsb en tar1090 bekijken;
- configuratie en database als back-up downloaden;
- oude historie verwijderen en SQLite opschonen;
- beheerderswachtwoord wijzigen.

Systeemacties lopen via een beperkt root-helperprogramma en een afzonderlijke sudoers-regel. De webservice krijgt geen algemene rootrechten.

## Beheer via SSH

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
git checkout feature/v0.8-webadmin
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
- Tracks met playback en hoogte-/snelheidsprofielen
- OpenStreetMap, topografische en satellietkaart
- Ontvangstanalyse en polarplot
- Filters voor heavies, helikopters, militair verkeer en noodsituaties
- Detectie van squawk 7500, 7600 en 7700
- Browsermeldingen en lokale waarschuwingen
- SQLite-historie, statistieken en CSV-export
- Raspberry Pi-systeemstatus
- Beveiligd webbeheer op `/admin`

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
- `/api/admin/me`
- `/live`
- `/health`

## Problemen controleren

```bash
sudo adsb-suite-doctor
sudo journalctl -u adsb-suite -n 100 --no-pager
```
