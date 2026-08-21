#!/usr/bin/env python3
"""Publish readsb aircraft data for the ESP32-C3-LCDkit ADS-B display.

The publisher deliberately runs as a small sidecar so it does not change the
existing ADS-B Suite web/API process. Configuration is done with environment
variables; see firmware/esp32-c3-lcdkit-adsb/README.md.
"""

from __future__ import annotations

import json
import math
import os
import signal
import sys
import time
from pathlib import Path
from typing import Any

import paho.mqtt.client as mqtt

SOURCE_FILE = Path(os.getenv("ADSB_SOURCE_FILE", "/run/readsb/aircraft.json"))
RECEIVER_LAT = float(os.getenv("ADSB_RECEIVER_LAT", "51.843"))
RECEIVER_LON = float(os.getenv("ADSB_RECEIVER_LON", "4.690"))
POLL_SECONDS = float(os.getenv("ADSB_DISPLAY_POLL", "2"))
ONLINE_MAX_AGE = float(os.getenv("ADSB_SOURCE_MAX_AGE", "15"))

MQTT_HOST = os.getenv("ADSB_MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.getenv("ADSB_MQTT_PORT", "1883"))
MQTT_USER = os.getenv("ADSB_MQTT_USER", "")
MQTT_PASSWORD = os.getenv("ADSB_MQTT_PASSWORD", "")
MQTT_BASE = os.getenv("ADSB_MQTT_BASE", "adsb/display").rstrip("/")
MQTT_CLIENT_ID = os.getenv("ADSB_MQTT_CLIENT_ID", "adsb-suite-display-publisher")

MAX_AIRCRAFT = int(os.getenv("ADSB_DISPLAY_MAX_AIRCRAFT", "24"))
VISIBLE_SEEN_SECONDS = float(os.getenv("ADSB_DISPLAY_MAX_SEEN", "15"))
ALERT_REPEAT_SECONDS = float(os.getenv("ADSB_ALERT_REPEAT", "300"))

EMERGENCY_SQUAWKS = {
    "7500": "HIJACK",
    "7600": "RADIO",
    "7700": "EMERGENCY",
}

running = True
last_message_count: int | None = None
last_message_time: float | None = None
alert_last_sent: dict[tuple[str, str], float] = {}


def stop(*_: Any) -> None:
    global running
    running = False


def haversine_and_bearing(lat1: float, lon1: float, lat2: float, lon2: float) -> tuple[float, float]:
    r_km = 6371.0088
    p1 = math.radians(lat1)
    p2 = math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)

    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    distance = r_km * 2 * math.atan2(math.sqrt(a), math.sqrt(max(0.0, 1 - a)))

    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    bearing = (math.degrees(math.atan2(y, x)) + 360.0) % 360.0
    return distance, bearing


def number(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def altitude_ft(value: Any) -> int:
    if isinstance(value, str) and value.lower() == "ground":
        return 0
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return 0


def clean_flight(value: Any) -> str:
    return str(value or "").strip()[:9]


def make_aircraft(raw: dict[str, Any]) -> dict[str, Any] | None:
    seen = number(raw.get("seen"), 9999)
    if seen > VISIBLE_SEEN_SECONDS:
        return None

    try:
        lat = float(raw["lat"])
        lon = float(raw["lon"])
    except (KeyError, TypeError, ValueError):
        return None

    distance_km, bearing = haversine_and_bearing(RECEIVER_LAT, RECEIVER_LON, lat, lon)
    return {
        "hex": str(raw.get("hex", ""))[:7],
        "flight": clean_flight(raw.get("flight")),
        "distance_km": round(distance_km, 2),
        "bearing": round(bearing, 1),
        "altitude_ft": altitude_ft(raw.get("alt_baro", raw.get("alt_geom"))),
        "speed_kt": round(number(raw.get("gs")), 1),
        "track": int(round(number(raw.get("track")))) % 360,
        "squawk": str(raw.get("squawk", ""))[:4],
        "seen": round(seen, 1),
    }


def publish_json(client: mqtt.Client, leaf: str, payload: Any, retain: bool = False) -> None:
    text = json.dumps(payload, separators=(",", ":"), ensure_ascii=True)
    info = client.publish(f"{MQTT_BASE}/{leaf}", text, qos=0, retain=retain)
    if info.rc != mqtt.MQTT_ERR_SUCCESS:
        raise RuntimeError(f"MQTT publish failed rc={info.rc} topic={leaf}")


def calculate_msg_rate(message_count: int, now: float) -> float:
    global last_message_count, last_message_time
    rate = 0.0
    if last_message_count is not None and last_message_time is not None:
        dt = now - last_message_time
        delta = message_count - last_message_count
        if dt > 0 and delta >= 0:
            rate = delta / dt
    last_message_count = message_count
    last_message_time = now
    return rate


def maybe_publish_alerts(client: mqtt.Client, aircraft: list[dict[str, Any]], now: float) -> None:
    active_keys: set[tuple[str, str]] = set()
    for a in aircraft:
        sq = a.get("squawk", "")
        if sq not in EMERGENCY_SQUAWKS:
            continue
        key = (a.get("hex", ""), sq)
        active_keys.add(key)
        last = alert_last_sent.get(key, 0.0)
        if now - last < ALERT_REPEAT_SECONDS:
            continue
        alert_last_sent[key] = now
        publish_json(
            client,
            "alert",
            {
                "event": EMERGENCY_SQUAWKS[sq],
                "hex": a.get("hex", ""),
                "flight": a.get("flight", ""),
                "squawk": sq,
                "distance_km": a.get("distance_km", 0),
                "altitude_ft": a.get("altitude_ft", 0),
                "timestamp": int(time.time()),
            },
            retain=False,
        )

    # Forget disappeared emergencies so a later reappearance can alert quickly.
    for key in list(alert_last_sent):
        if key not in active_keys and now - alert_last_sent[key] > 60:
            alert_last_sent.pop(key, None)


def read_source() -> tuple[dict[str, Any], float]:
    stat = SOURCE_FILE.stat()
    age = max(0.0, time.time() - stat.st_mtime)
    with SOURCE_FILE.open("r", encoding="utf-8") as fh:
        return json.load(fh), age


def main() -> int:
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=MQTT_CLIENT_ID, clean_session=True)
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    client.will_set(f"{MQTT_BASE}/publisher", "offline", qos=0, retain=True)
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_start()
    client.publish(f"{MQTT_BASE}/publisher", "online", qos=0, retain=True)

    print(f"ADS-B display MQTT publisher: {SOURCE_FILE} -> mqtt://{MQTT_HOST}:{MQTT_PORT}/{MQTT_BASE}")

    try:
        while running:
            started = time.monotonic()
            now = time.monotonic()
            source_online = False
            source_age = 9999.0
            data: dict[str, Any] = {}

            try:
                data, source_age = read_source()
                source_online = source_age <= ONLINE_MAX_AGE
            except (OSError, json.JSONDecodeError) as exc:
                print(f"source read error: {exc}", file=sys.stderr)

            message_count = int(number(data.get("messages"), 0))
            msg_rate = calculate_msg_rate(message_count, now) if data else 0.0

            raw_aircraft = data.get("aircraft", []) if isinstance(data.get("aircraft", []), list) else []
            positioned: list[dict[str, Any]] = []
            visible_count = 0
            for raw in raw_aircraft:
                if not isinstance(raw, dict):
                    continue
                if number(raw.get("seen"), 9999) <= VISIBLE_SEEN_SECONDS:
                    visible_count += 1
                parsed = make_aircraft(raw)
                if parsed is not None:
                    positioned.append(parsed)

            positioned.sort(key=lambda a: a["distance_km"])
            max_range = max((a["distance_km"] for a in positioned), default=0.0)
            nearest = positioned[0] if positioned else {}
            display_aircraft = positioned[:MAX_AIRCRAFT]

            summary = {
                "aircraft": visible_count,
                "with_position": len(positioned),
                "msg_rate": round(msg_rate, 1),
                "max_range_km": round(max_range, 1),
                "source_online": source_online,
                "age_seconds": int(round(source_age)),
                "nearest": nearest,
                "timestamp": int(time.time()),
            }

            try:
                publish_json(client, "summary", summary, retain=True)
                publish_json(client, "aircraft", display_aircraft, retain=True)
                maybe_publish_alerts(client, positioned, time.monotonic())
            except RuntimeError as exc:
                print(exc, file=sys.stderr)

            elapsed = time.monotonic() - started
            time.sleep(max(0.1, POLL_SECONDS - elapsed))
    finally:
        client.publish(f"{MQTT_BASE}/publisher", "offline", qos=0, retain=True)
        client.loop_stop()
        client.disconnect()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
