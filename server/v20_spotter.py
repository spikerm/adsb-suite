#!/usr/bin/env python3
"""Spotter Pro v2 API for ADS-B Suite."""
import asyncio
import json
import sqlite3
import time
import urllib.request
from pathlib import Path
from aiohttp import web

DEFAULTS = {
    "enabled": True,
    "browser_notifications": True,
    "sound": True,
    "max_distance_km": 25,
    "max_altitude_ft": 12000,
    "military": True,
    "helicopters": True,
    "emergency_squawks": True,
    "registrations": ["PH-"],
    "types": ["A400", "C17", "C130", "CH47", "AH64"],
    "callsigns": [],
    "webhook_url": "",
    "cooldown_minutes": 15
}


def register_spotter(app, db_path, cfg):
    db_path = Path(db_path)
    settings_path = Path(cfg.get("spotter_settings_path", "/var/lib/adsb-suite/spotter.json"))
    settings_path.parent.mkdir(parents=True, exist_ok=True)

    def connect():
        conn = sqlite3.connect(db_path, timeout=20)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA busy_timeout=5000")
        conn.execute("""CREATE TABLE IF NOT EXISTS spotter_events(
          id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, hex TEXT, flight TEXT,
          registration TEXT, aircraft_type TEXT, operator TEXT, reason TEXT,
          distance_km REAL, altitude_ft REAL, squawk TEXT, payload TEXT
        )""")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_spotter_events_ts ON spotter_events(ts)")
        return conn

    def load_settings():
        data = dict(DEFAULTS)
        try:
            if settings_path.exists():
                saved = json.loads(settings_path.read_text(encoding="utf-8"))
                if isinstance(saved, dict):
                    data.update(saved)
        except (OSError, json.JSONDecodeError):
            pass
        return data

    def save_settings(data):
        clean = dict(DEFAULTS)
        for key in clean:
            if key in data:
                clean[key] = data[key]
        clean["max_distance_km"] = max(1, min(500, float(clean["max_distance_km"])))
        clean["max_altitude_ft"] = max(0, min(60000, float(clean["max_altitude_ft"])))
        clean["cooldown_minutes"] = max(1, min(1440, int(clean["cooldown_minutes"])))
        for key in ("registrations", "types", "callsigns"):
            clean[key] = [str(x).strip().upper() for x in clean.get(key, []) if str(x).strip()][:100]
        clean["webhook_url"] = str(clean.get("webhook_url") or "").strip()[:1000]
        tmp = settings_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(clean, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        tmp.replace(settings_path)
        return clean

    async def get_settings(_request):
        return web.json_response(load_settings(), headers={"Cache-Control": "no-store"})

    async def put_settings(request):
        try:
            body = await request.json()
            if not isinstance(body, dict):
                raise ValueError("object verwacht")
            data = save_settings(body)
            return web.json_response({"ok": True, "settings": data})
        except (ValueError, TypeError, json.JSONDecodeError) as exc:
            raise web.HTTPBadRequest(text=f"Ongeldige instellingen: {exc}")

    async def event(request):
        body = await request.json()
        if not isinstance(body, dict):
            raise web.HTTPBadRequest(text="JSON-object verwacht")
        now = int(time.time())
        with connect() as conn:
            conn.execute("""INSERT INTO spotter_events
              (ts,hex,flight,registration,aircraft_type,operator,reason,distance_km,altitude_ft,squawk,payload)
              VALUES(?,?,?,?,?,?,?,?,?,?,?)""", (
                now, body.get("hex"), body.get("flight"), body.get("registration"),
                body.get("aircraft_type"), body.get("operator"), body.get("reason"),
                body.get("distance_km"), body.get("altitude_ft"), body.get("squawk"),
                json.dumps(body, ensure_ascii=False)[:20000]
            ))
        settings = load_settings()
        webhook = settings.get("webhook_url")
        webhook_status = "disabled"
        if webhook:
            def send():
                req = urllib.request.Request(webhook, data=json.dumps(body).encode("utf-8"),
                    headers={"Content-Type": "application/json", "User-Agent": "ADS-B-Suite-Spotter"})
                with urllib.request.urlopen(req, timeout=8) as response:
                    return response.status
            try:
                webhook_status = await asyncio.to_thread(send)
            except Exception as exc:
                webhook_status = f"error: {type(exc).__name__}: {exc}"
        return web.json_response({"ok": True, "webhook": webhook_status})

    async def stats(request):
        try:
            hours = max(1, min(24 * 365, int(request.query.get("hours", 168))))
        except ValueError:
            raise web.HTTPBadRequest(text="Ongeldige hours")
        since = int(time.time()) - hours * 3600
        with connect() as conn:
            summary = dict(conn.execute("""SELECT COUNT(*) events, COUNT(DISTINCT hex) aircraft,
              MIN(distance_km) nearest_km, MAX(distance_km) farthest_km
              FROM spotter_events WHERE ts>=?""", (since,)).fetchone())
            reasons = [dict(r) for r in conn.execute("""SELECT reason,COUNT(*) count FROM spotter_events
              WHERE ts>=? GROUP BY reason ORDER BY count DESC LIMIT 20""", (since,))]
            recent = [dict(r) for r in conn.execute("""SELECT ts,hex,flight,registration,aircraft_type,
              operator,reason,distance_km,altitude_ft,squawk FROM spotter_events
              WHERE ts>=? ORDER BY ts DESC LIMIT 100""", (since,))]
        return web.json_response({"hours": hours, "summary": summary, "reasons": reasons, "recent": recent},
                                 headers={"Cache-Control": "no-store"})

    app.router.add_get("/api/spotter/settings", get_settings)
    app.router.add_put("/api/spotter/settings", put_settings)
    app.router.add_post("/api/spotter/event", event)
    app.router.add_get("/api/spotter/stats", stats)
