# ESP32-C3-LCDkit ADS-B display

Round 240x240 ADS-B status/radar display for the Espressif ESP32-C3-LCDkit.

Hardware pin mapping follows the official ESP32-C3-LCDkit revision:

- GC9A01 LCD: MOSI GPIO0, SCLK GPIO1, D/C GPIO2, CS GPIO7, backlight GPIO5
- EC11 encoder: A GPIO10, B GPIO6, switch GPIO9
- WS2812 RGB LED: GPIO8

The firmware uses Wi-Fi and MQTT. ADS-B decoding remains on the Raspberry Pi/readsb; `server/adsb_display_mqtt.py` converts `/run/readsb/aircraft.json` into compact display topics.

## Screens

1. Overview — visible aircraft, positions, message rate and current maximum range
2. Radar — nearest positioned aircraft on the round display; rotate to select 25/50/100/200/400 km
3. Nearest — callsign/ICAO, distance, altitude, speed and track
4. Receiver — source, Wi-Fi, MQTT and data-age status

Emergency squawks 7500, 7600 and 7700 create an alert overlay and red RGB indication.

## MQTT topics

Default base topic: `adsb/display`

- `adsb/display/summary` retained JSON
- `adsb/display/aircraft` retained JSON array, nearest first, maximum 24
- `adsb/display/alert` transient JSON event
- `adsb/display/publisher` retained `online` / `offline`
- `adsb/display/status` retained ESP32 `online` / `offline`

## 1. Configure the ESP32

This project is made for PlatformIO.

```bash
cd firmware/esp32-c3-lcdkit-adsb
cp include/config.example.h include/config.h
```

Edit `include/config.h` and set Wi-Fi and MQTT details, then build/upload:

```bash
pio run
pio run -t upload
pio device monitor
```

If automatic flashing does not start, hold the rotary encoder switch, press/release Reset, then release the encoder switch. This puts the ESP32-C3 into download mode.

## 2. Run the Raspberry Pi MQTT publisher

Install the server requirements in the ADS-B Suite virtual environment:

```bash
pip install -r server/requirements.txt
```

Example launch:

```bash
export ADSB_RECEIVER_LAT=51.843
export ADSB_RECEIVER_LON=4.690
export ADSB_MQTT_HOST=127.0.0.1
export ADSB_MQTT_PORT=1883
python3 server/adsb_display_mqtt.py
```

Optional environment variables:

```text
ADSB_SOURCE_FILE=/run/readsb/aircraft.json
ADSB_MQTT_USER=
ADSB_MQTT_PASSWORD=
ADSB_MQTT_BASE=adsb/display
ADSB_DISPLAY_POLL=2
ADSB_DISPLAY_MAX_AIRCRAFT=24
ADSB_DISPLAY_MAX_SEEN=15
ADSB_SOURCE_MAX_AGE=15
ADSB_ALERT_REPEAT=300
```

## Example `summary`

```json
{
  "aircraft": 37,
  "with_position": 29,
  "msg_rate": 284.8,
  "max_range_km": 311.7,
  "source_online": true,
  "age_seconds": 1,
  "nearest": {
    "hex": "484abc",
    "flight": "KLM123",
    "distance_km": 12.6,
    "bearing": 42.1,
    "altitude_ft": 18400,
    "speed_kt": 326.0,
    "track": 217,
    "squawk": "1000"
  }
}
```

## Notes

The GC9A01 display is configured directly through TFT_eSPI build flags, so no local modification of the TFT_eSPI library is required. `config.h` contains credentials and should not be committed.
