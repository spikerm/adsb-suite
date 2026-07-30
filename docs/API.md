# API v0.2

Alle JSON-responses gebruiken SI/aviation velden zoals `distance_km`, `altitude_ft`, `speed_kt` en `track_deg`.

`GET /api/status` geeft bronstatus, versie, aantallen en ontvangerpositie.

`GET /api/aircraft` geeft alle live vliegtuigen met positie, afstand en verrijkte metadata.

`GET /api/history?hours=24&hex=<icao>` geeft maximaal 20.000 historische observaties.

`GET /api/aircraft/<icao>` geeft de samenvatting en maximaal 1.000 trackpunten.

`GET /api/search?q=<term>` zoekt op ICAO-hex, callsign, registratie, type en operator.

`GET /api/export.csv?hours=24` exporteert maximaal 100.000 observaties.

`WS /live` stuurt berichten als `{"type":"aircraft","data":{...}}`.
