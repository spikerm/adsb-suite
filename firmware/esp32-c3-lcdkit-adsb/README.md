# ESP32-C3-LCDkit ADS-B display

Round 240x240 ADS-B status/radar display for the Espressif ESP32-C3-LCDkit.

This version uses **ESP-IDF plus Espressif's official `esp32_c3_lcdkit` BSP**. The GC9A01 display is no longer initialized through generic TFT_eSPI code. The official BSP owns the LCD SPI bus, GC9A01 panel setup, LVGL integration and backlight control.

Hardware is taken from the official BSP:

- GC9A01 LCD: MOSI GPIO0, SCLK GPIO1, D/C GPIO2, CS GPIO7, backlight GPIO5
- EC11 encoder: A GPIO10, B GPIO6, switch GPIO9
- WS2812 RGB LED: GPIO8

The firmware uses Wi-Fi and MQTT. ADS-B decoding remains on the Raspberry Pi/readsb; `server/adsb_display_mqtt.py` converts `/run/readsb/aircraft.json` into compact display topics.

## Screens

1. Overview — visible aircraft, positions, message rate, maximum range and nearest aircraft
2. Radar — nearest positioned aircraft; rotate the encoder to select 25/50/100/200/400 km
3. Nearest — callsign/ICAO, distance, altitude, speed, track and squawk
4. Receiver — ADS-B source, Wi-Fi, MQTT and data-age status

Emergency squawks 7500, 7600 and 7700 are highlighted on the radar and can show an alert banner.

## MQTT topics

Default base topic: `adsb/display`

- `adsb/display/summary` retained JSON
- `adsb/display/aircraft` retained JSON array, nearest first, maximum 24
- `adsb/display/alert` transient JSON event
- `adsb/display/publisher` retained `online` / `offline`
- `adsb/display/status` retained ESP32 `online` / `offline`

## Configure

Copy the example credentials file once:

```bash
cd firmware/esp32-c3-lcdkit-adsb
cp include/config.example.h include/config.h
```

On Windows PowerShell:

```powershell
Copy-Item include\config.example.h include\config.h
```

Edit `include/config.h`. For the current Raspberry Pi setup the MQTT host is `10.10.0.114`, port `1883`, user `adsb-display` and base topic `adsb/display`.

## Build with PlatformIO

The PlatformIO project now uses:

```ini
framework = espidf
```

The IDF Component Manager automatically downloads the official Espressif `esp32_c3_lcdkit` BSP and its LVGL/GC9A01 dependencies.

After changing from the previous Arduino/TFT_eSPI build, do a clean build once:

```powershell
pio run -t clean
pio run
```

Upload:

```powershell
pio run -t upload
```

Monitor:

```powershell
pio device monitor
```

Expected startup log contains roughly:

```text
ADS-B LCDkit ESP-IDF/BSP build starting
Starting official ESP32-C3-LCDkit BSP display
Starting Wi-Fi
Wi-Fi connected
MQTT connected
```

If automatic flashing does not start, place the ESP32-C3 in download mode and retry the upload.

## Raspberry Pi publisher

Example manual launch:

```bash
export ADSB_RECEIVER_LAT=51.843
export ADSB_RECEIVER_LON=4.690
export ADSB_MQTT_HOST=127.0.0.1
export ADSB_MQTT_PORT=1883
export ADSB_MQTT_USER=adsb-display
export ADSB_MQTT_PASSWORD='your-password'
python3 server/adsb_display_mqtt.py
```

In the deployed Raspberry Pi setup this publisher runs as `adsb-display-mqtt.service`.

## Architecture

```text
RTL-SDR -> readsb -> aircraft.json -> adsb_display_mqtt.py -> Mosquitto -> ESP32-C3-LCDkit
                                                                  |
                                                                  +-> LVGL radar/status UI
```

`config.h` contains credentials and should not be committed.
