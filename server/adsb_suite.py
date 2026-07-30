#!/usr/bin/env python3
"""ADS-B Suite v0.3.0 beta 2: readsb ingest, metadata, system status, history, tracks and dashboard."""
import asyncio
import csv
import io
import json
import math
import os
import shutil
import sqlite3
import time
from pathlib import Path
from typing import Any

from aiohttp import WSMsgType, web

VERSION = "0.3.0-beta2"
BASE = Path(__file__).resolve().parent
CONFIG_PATH = Path(os.environ.get("ADSB_SUITE_CONFIG", "/etc/adsb-suite/config.json"))
DEFAULT: dict[str, Any] = {
    "listen_host": "0.0.0.0",
    "listen_port": 8090,
    "source_file": "/run/readsb/aircraft.json",
    "receiver_lat": 51.843,
    "receiver_lon": 4.690,
    "receiver_name": "ADS-B Receiver",
    "poll_interval_seconds": 2,
    "history_interval_seconds": 10,
    "retention_days": 90,
    "track_default_minutes": 30,
    "track_max_hours": 24,
    "database_path": "/var/lib/adsb-suite/adsb-suite.db",
    "aircraft_database_path": "/var/lib/adsb-suite/aircraft.csv",
}


def load_config() -> dict[str, Any]:
    cfg = DEFAULT.copy()
    try:
        if CONFIG_PATH.exists():
            cfg.update(json.loads(CONFIG_PATH.read_text(encoding="utf-8")))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"Kan configuratie {CONFIG_PATH} niet lezen: {exc}") from exc
    return cfg


CFG = load_config()
DB = Path(CFG["database_path"])
DB.parent.mkdir(parents=True, exist_ok=True)
SOURCE = Path(CFG["source_file"])
state: dict[str, Any] = {
    "updated_at": 0,
    "source_generated_at": 0,
    "aircraft": [],
    "source_ok": False,
    "source_error": None,
    "messages": 0,
    "last_messages": 0,
    "last_message_ts": 0.0,
    "messages_per_second": 0.0,
}
websockets: set[web.WebSocketResponse] = set()
aircraft_db: dict[str, dict[str, str]] = {}


def dbconn() -> sqlite3.Connection:
    conn = sqlite3.connect(DB, timeout=20)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("PRAGMA busy_timeout=5000")
    return conn


def add_missing_columns(conn: sqlite3.Connection, table: str, columns: dict[str, str]) -> None:
    existing = {row[1] for row in conn.execute(f"PRAGMA table_info({table})")}
    for name, sql_type in columns.items():
        if name not in existing:
            conn.execute(f"ALTER TABLE {table} ADD COLUMN {name} {sql_type}")


def init_db() -> None:
    with dbconn() as conn:
        conn.executescript("""
        CREATE TABLE IF NOT EXISTS observations(
          id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, hex TEXT NOT NULL,
          flight TEXT, registration TEXT, aircraft_type TEXT, description TEXT, operator TEXT,
          manufacturer TEXT, model TEXT, country TEXT,
          lat REAL, lon REAL, altitude_ft REAL, speed_kt REAL, track_deg REAL,
          vertical_rate_fpm REAL, distance_km REAL, bearing_deg REAL,
          squawk TEXT, emergency TEXT, category TEXT, rssi REAL,
          UNIQUE(ts, hex)
        );
        CREATE INDEX IF NOT EXISTS idx_obs_ts ON observations(ts);
        CREATE INDEX IF NOT EXISTS idx_obs_hex_ts ON observations(hex, ts);
        CREATE INDEX IF NOT EXISTS idx_obs_flight ON observations(flight);
        CREATE TABLE IF NOT EXISTS seen_aircraft(
          hex TEXT PRIMARY KEY, first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL,
          flight TEXT, registration TEXT, aircraft_type TEXT, description TEXT, operator TEXT,
          manufacturer TEXT, model TEXT, country TEXT,
          observations INTEGER NOT NULL DEFAULT 0, min_distance_km REAL,
          max_altitude_ft REAL, max_speed_kt REAL
        );
        """)
        add_missing_columns(conn, "observations", {
            "description": "TEXT", "operator": "TEXT", "manufacturer": "TEXT",
            "model": "TEXT", "country": "TEXT",
        })
        add_missing_columns(conn, "seen_aircraft", {
            "description": "TEXT", "operator": "TEXT", "manufacturer": "TEXT",
            "model": "TEXT", "country": "TEXT",
        })


def clean_metadata(row: dict[str, str]) -> dict[str, str]:
    return {str(k).strip().lower(): str(v or "").strip() for k, v in row.items()}


def load_aircraft_db() -> None:
    aircraft_db.clear()
    path = Path(CFG["aircraft_database_path"])
    if not path.exists():
        return
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as handle:
            for raw in csv.DictReader(handle):
                row = clean_metadata(raw)
                hx = (row.get("hex") or row.get("icao24") or row.get("icao") or "").lower()
                if hx:
                    aircraft_db[hx] = row
    except OSError as exc:
        print(f"aircraft database error: {exc}", flush=True)


def finite_number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)) and math.isfinite(float(value)):
        return float(value)
    return None


def first_number(*values: Any) -> float | None:
    for value in values:
        number = finite_number(value)
        if number is not None:
            return number
    return None


def first_text(*values: Any) -> str | None:
    for value in values:
        text = str(value or "").strip()
        if text:
            return text
    return None


def haversine(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    radius = 6371.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = math.radians(lat2 - lat1), math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * radius * math.atan2(math.sqrt(a), math.sqrt(1 - a))


def bearing(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    p1, p2, dl = math.radians(lat1), math.radians(lat2), math.radians(lon2 - lon1)
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return (math.degrees(math.atan2(y, x)) + 360) % 360


def normalize(payload: dict[str, Any]) -> list[dict[str, Any]]:
    raw = payload.get("aircraft", [])
    if not isinstance(raw, list):
        return []
    result: list[dict[str, Any]] = []
    receiver_lat = float(CFG["receiver_lat"])
    receiver_lon = float(CFG["receiver_lon"])
    for item in raw:
        if not isinstance(item, dict):
            continue
        lat, lon = finite_number(item.get("lat")), finite_number(item.get("lon"))
        if lat is None or lon is None:
            continue
        hx = str(item.get("hex") or "").strip().lower()
        if not hx:
            continue
        metadata = aircraft_db.get(hx, {})
        altitude = first_number(item.get("alt_baro"), item.get("alt_geom"))
        speed = first_number(item.get("gs"), item.get("tas"))
        track = first_number(item.get("track"), item.get("true_heading"))
        vertical_rate = first_number(item.get("baro_rate"), item.get("geom_rate"))
        distance = haversine(receiver_lat, receiver_lon, lat, lon)
        model = first_text(metadata.get("model"), metadata.get("description"))
        manufacturer = first_text(metadata.get("manufacturer"), metadata.get("maker"))
        row = dict(item)
        row.update({
            "hex": hx,
            "flight": str(item.get("flight") or "").strip(),
            "registration": first_text(item.get("registration"), item.get("r"), metadata.get("registration"), metadata.get("reg")),
            "aircraft_type": first_text(item.get("aircraft_type"), item.get("t"), metadata.get("type"), metadata.get("icao_type"), item.get("type")),
            "description": first_text(metadata.get("description"), model),
            "operator": first_text(metadata.get("operator"), metadata.get("owner"), metadata.get("airline")),
            "manufacturer": manufacturer,
            "model": model,
            "country": first_text(metadata.get("country"), metadata.get("country_name")),
            "lat": lat, "lon": lon, "altitude_ft": altitude, "speed_kt": speed,
            "track_deg": track, "vertical_rate_fpm": vertical_rate,
            "distance_km": round(distance, 3),
            "bearing_deg": round(bearing(receiver_lat, receiver_lon, lat, lon), 2),
            "seen_seconds": finite_number(item.get("seen")),
        })
        result.append(row)
    result.sort(key=lambda aircraft: aircraft["distance_km"])
    return result


def read_source_sync() -> dict[str, Any]:
    error: Exception | None = None
    for _ in range(3):
        try:
            return json.loads(SOURCE.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            error = exc
            time.sleep(0.05)
    raise RuntimeError(f"Kan {SOURCE} niet lezen: {error}")


def snapshot(include_aircraft: bool = True) -> dict[str, Any]:
    aircraft = state["aircraft"]
    data: dict[str, Any] = {
        "now": int(time.time()), "updated_at": state["updated_at"],
        "source_generated_at": state["source_generated_at"], "source_ok": state["source_ok"],
        "source_error": state["source_error"], "messages": state["messages"],
        "messages_per_second": round(float(state["messages_per_second"]), 1),
        "count": len(aircraft), "positioned_count": len(aircraft),
        "receiver": {"name": CFG["receiver_name"], "lat": CFG["receiver_lat"], "lon": CFG["receiver_lon"]},
        "nearest": aircraft[0] if aircraft else None,
    }
    if include_aircraft:
        data["aircraft"] = aircraft
    return data


async def broadcast(data: dict[str, Any]) -> None:
    text = json.dumps(data, separators=(",", ":"), ensure_ascii=False)
    dead: list[web.WebSocketResponse] = []
    for ws in tuple(websockets):
        try:
            await ws.send_str(text)
        except Exception:
            dead.append(ws)
    for ws in dead:
        websockets.discard(ws)


async def poller(_: web.Application) -> None:
    while True:
        try:
            payload = await asyncio.to_thread(read_source_sync)
            messages = int(payload.get("messages") or 0)
            now = time.monotonic()
            if state["last_message_ts"] and messages >= state["last_messages"]:
                elapsed = max(now - float(state["last_message_ts"]), 0.001)
                state["messages_per_second"] = (messages - int(state["last_messages"])) / elapsed
            state["last_messages"], state["last_message_ts"], state["messages"] = messages, now, messages
            state["aircraft"] = normalize(payload)
            state["source_generated_at"] = int(payload.get("now") or 0)
            state.update(updated_at=int(time.time()), source_ok=True, source_error=None)
            await broadcast({"type": "aircraft", "data": snapshot()})
        except Exception as exc:
            state.update(source_ok=False, source_error=str(exc))
        await asyncio.sleep(max(0.5, float(CFG["poll_interval_seconds"])))


def store_history_sync() -> None:
    ts = int(time.time())
    rows = []
    for aircraft in state["aircraft"]:
        if not aircraft.get("hex"):
            continue
        rows.append((
            ts, aircraft.get("hex"), aircraft.get("flight"), aircraft.get("registration"),
            aircraft.get("aircraft_type"), aircraft.get("description"), aircraft.get("operator"),
            aircraft.get("manufacturer"), aircraft.get("model"), aircraft.get("country"),
            aircraft.get("lat"), aircraft.get("lon"), aircraft.get("altitude_ft"), aircraft.get("speed_kt"),
            aircraft.get("track_deg"), aircraft.get("vertical_rate_fpm"), aircraft.get("distance_km"),
            aircraft.get("bearing_deg"), aircraft.get("squawk"), aircraft.get("emergency"),
            aircraft.get("category"), aircraft.get("rssi"),
        ))
    with dbconn() as conn:
        conn.executemany("""INSERT OR IGNORE INTO observations(
          ts,hex,flight,registration,aircraft_type,description,operator,manufacturer,model,country,
          lat,lon,altitude_ft,speed_kt,track_deg,vertical_rate_fpm,distance_km,bearing_deg,
          squawk,emergency,category,rssi
        ) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""", rows)
        for row in rows:
            (ts_, hx, flight, registration, aircraft_type, description, operator, manufacturer,
             model, country, _lat, _lon, altitude, speed, _track, _vr, distance, *_) = row
            conn.execute("""INSERT INTO seen_aircraft(
              hex,first_seen,last_seen,flight,registration,aircraft_type,description,operator,
              manufacturer,model,country,observations,min_distance_km,max_altitude_ft,max_speed_kt
            ) VALUES(?,?,?,?,?,?,?,?,?,?,?,1,?,?,?)
            ON CONFLICT(hex) DO UPDATE SET
              last_seen=excluded.last_seen,
              flight=COALESCE(NULLIF(excluded.flight,''),seen_aircraft.flight),
              registration=COALESCE(excluded.registration,seen_aircraft.registration),
              aircraft_type=COALESCE(excluded.aircraft_type,seen_aircraft.aircraft_type),
              description=COALESCE(excluded.description,seen_aircraft.description),
              operator=COALESCE(excluded.operator,seen_aircraft.operator),
              manufacturer=COALESCE(excluded.manufacturer,seen_aircraft.manufacturer),
              model=COALESCE(excluded.model,seen_aircraft.model),
              country=COALESCE(excluded.country,seen_aircraft.country),
              observations=seen_aircraft.observations+1,
              min_distance_km=MIN(COALESCE(seen_aircraft.min_distance_km,excluded.min_distance_km),excluded.min_distance_km),
              max_altitude_ft=MAX(COALESCE(seen_aircraft.max_altitude_ft,excluded.max_altitude_ft),excluded.max_altitude_ft),
              max_speed_kt=MAX(COALESCE(seen_aircraft.max_speed_kt,excluded.max_speed_kt),excluded.max_speed_kt)
            """, (hx, ts_, ts_, flight, registration, aircraft_type, description, operator,
                  manufacturer, model, country, distance, altitude, speed))
        conn.execute("DELETE FROM observations WHERE ts < ?", (ts - int(CFG["retention_days"]) * 86400,))


async def historian(_: web.Application) -> None:
    while True:
        try:
            await asyncio.to_thread(store_history_sync)
        except Exception as exc:
            print(f"history error: {exc}", flush=True)
        await asyncio.sleep(max(5, float(CFG["history_interval_seconds"])))


def query_int(request: web.Request, name: str, default: int, minimum: int, maximum: int) -> int:
    try:
        return min(max(int(request.query.get(name, str(default))), minimum), maximum)
    except ValueError:
        return default


def read_cpu_temperature() -> float | None:
    for path in (Path("/sys/class/thermal/thermal_zone0/temp"), Path("/sys/devices/virtual/thermal/thermal_zone0/temp")):
        try:
            return round(float(path.read_text().strip()) / 1000.0, 1)
        except (OSError, ValueError):
            continue
    return None


def read_memory() -> dict[str, Any]:
    values: dict[str, int] = {}
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            key, value = line.split(":", 1)
            values[key] = int(value.strip().split()[0]) * 1024
    except (OSError, ValueError):
        return {"total_bytes": None, "available_bytes": None, "used_percent": None}
    total, available = values.get("MemTotal"), values.get("MemAvailable")
    used = round((1 - available / total) * 100, 1) if total and available is not None else None
    return {"total_bytes": total, "available_bytes": available, "used_percent": used}


def system_status_sync() -> dict[str, Any]:
    disk = shutil.disk_usage("/")
    load1, load5, load15 = os.getloadavg()
    uptime = None
    try:
        uptime = round(float(Path("/proc/uptime").read_text().split()[0]))
    except (OSError, ValueError, IndexError):
        pass
    return {
        "cpu_temperature_c": read_cpu_temperature(),
        "load_average": {"1m": round(load1, 2), "5m": round(load5, 2), "15m": round(load15, 2)},
        "memory": read_memory(),
        "disk": {
            "total_bytes": disk.total, "used_bytes": disk.used, "free_bytes": disk.free,
            "used_percent": round(disk.used / disk.total * 100, 1) if disk.total else None,
        },
        "uptime_seconds": uptime,
    }


async def api_status(_: web.Request) -> web.Response:
    with dbconn() as conn:
        observations = conn.execute("SELECT COUNT(*) FROM observations").fetchone()[0]
        unique = conn.execute("SELECT COUNT(*) FROM seen_aircraft").fetchone()[0]
    return web.json_response({
        "status": "ok" if state["source_ok"] else "degraded", "version": VERSION,
        "source_file": str(SOURCE), "database": str(DB), "observations": observations,
        "unique_aircraft": unique, **snapshot(False),
    }, headers={"Cache-Control": "no-store"})


async def api_aircraft(_: web.Request) -> web.Response:
    return web.json_response(snapshot(), headers={"Cache-Control": "no-store"})


async def api_nearest(_: web.Request) -> web.Response:
    return web.json_response({"nearest": snapshot(False)["nearest"]})


async def api_receiver(_: web.Request) -> web.Response:
    aircraft = state["aircraft"]
    return web.json_response({
        "receiver": snapshot(False)["receiver"], "source_ok": state["source_ok"],
        "source_file": str(SOURCE), "messages": state["messages"],
        "messages_per_second": round(float(state["messages_per_second"]), 1),
        "live_aircraft": len(aircraft),
        "current_max_range_km": round(max((float(a.get("distance_km") or 0) for a in aircraft), default=0.0), 1),
        "updated_at": state["updated_at"],
    })


async def api_system(_: web.Request) -> web.Response:
    return web.json_response(await asyncio.to_thread(system_status_sync), headers={"Cache-Control": "no-store"})


async def api_history(request: web.Request) -> web.Response:
    hours = query_int(request, "hours", 24, 1, 24 * 365)
    limit = query_int(request, "limit", 5000, 1, 20000)
    hx = (request.query.get("hex") or "").strip().lower()
    since = int(time.time()) - hours * 3600
    sql, params = "SELECT * FROM observations WHERE ts>=?", [since]
    if hx:
        sql += " AND hex=?"
        params.append(hx)
    sql += " ORDER BY ts DESC LIMIT ?"
    params.append(limit)
    with dbconn() as conn:
        rows = [dict(row) for row in conn.execute(sql, params)]
    return web.json_response({"count": len(rows), "hours": hours, "observations": rows})


async def api_track(request: web.Request) -> web.Response:
    hx = request.match_info["hex"].strip().lower()
    minutes = query_int(request, "minutes", int(CFG["track_default_minutes"]), 1, int(CFG["track_max_hours"]) * 60)
    limit = query_int(request, "limit", 2000, 10, 10000)
    since = int(time.time()) - minutes * 60
    with dbconn() as conn:
        summary = conn.execute("SELECT * FROM seen_aircraft WHERE hex=?", (hx,)).fetchone()
        rows = [dict(row) for row in conn.execute(
            """SELECT ts,lat,lon,altitude_ft,speed_kt,track_deg,vertical_rate_fpm,distance_km
               FROM observations WHERE hex=? AND ts>=? AND lat IS NOT NULL AND lon IS NOT NULL
               ORDER BY ts ASC LIMIT ?""", (hx, since, limit))]
    if not summary and not rows:
        return web.json_response({"error": "not_found", "hex": hx}, status=404)
    distance = sum(haversine(float(a["lat"]), float(a["lon"]), float(b["lat"]), float(b["lon"]))
                   for a, b in zip(rows, rows[1:]))
    altitudes = [float(row["altitude_ft"]) for row in rows if row["altitude_ft"] is not None]
    speeds = [float(row["speed_kt"]) for row in rows if row["speed_kt"] is not None]
    return web.json_response({
        "hex": hx, "minutes": minutes, "count": len(rows),
        "aircraft": dict(summary) if summary else None,
        "summary": {
            "distance_km": round(distance, 2),
            "max_altitude_ft": max(altitudes) if altitudes else None,
            "average_speed_kt": round(sum(speeds) / len(speeds), 1) if speeds else None,
            "first_ts": rows[0]["ts"] if rows else None, "last_ts": rows[-1]["ts"] if rows else None,
        }, "points": rows,
    }, headers={"Cache-Control": "no-store"})


async def api_stats(_: web.Request) -> web.Response:
    since = int(time.time()) - 86400
    with dbconn() as conn:
        summary = dict(conn.execute(
            """SELECT COUNT(*) observations,COUNT(DISTINCT hex) unique_aircraft,
            MIN(distance_km) min_distance_km,MAX(distance_km) max_distance_km,
            MAX(altitude_ft) max_altitude_ft,MAX(speed_kt) max_speed_kt
            FROM observations WHERE ts>=?""", (since,)).fetchone())
        hourly = [dict(row) for row in conn.execute(
            """SELECT strftime('%Y-%m-%dT%H:00:00Z',ts,'unixepoch') hour,
            COUNT(*) observations,COUNT(DISTINCT hex) unique_aircraft
            FROM observations WHERE ts>=? GROUP BY hour ORDER BY hour""", (since,))]
        top = [dict(row) for row in conn.execute(
            """SELECT hex,flight,registration,aircraft_type,operator,manufacturer,model,country,
            observations,min_distance_km,last_seen FROM seen_aircraft
            ORDER BY observations DESC LIMIT 20""")]
        top_operators = [dict(row) for row in conn.execute(
            """SELECT operator,COUNT(DISTINCT hex) aircraft,SUM(observations) observations
            FROM seen_aircraft WHERE operator IS NOT NULL AND operator!=''
            GROUP BY operator ORDER BY observations DESC LIMIT 10""")]
        top_types = [dict(row) for row in conn.execute(
            """SELECT COALESCE(NULLIF(model,''),aircraft_type) type,
            COUNT(DISTINCT hex) aircraft,SUM(observations) observations
            FROM seen_aircraft WHERE COALESCE(NULLIF(model,''),aircraft_type) IS NOT NULL
            GROUP BY type ORDER BY observations DESC LIMIT 10""")]
    return web.json_response({
        "period": "24h", "summary": summary, "hourly": hourly,
        "top_aircraft": top, "top_operators": top_operators, "top_types": top_types,
    })


async def api_live_stats(_: web.Request) -> web.Response:
    aircraft = state["aircraft"]
    return web.json_response({
        "live_aircraft": len(aircraft), "messages": state["messages"],
        "messages_per_second": round(float(state["messages_per_second"]), 1),
        "nearest_distance_km": aircraft[0]["distance_km"] if aircraft else None,
        "maximum_range_km": max((float(a.get("distance_km") or 0) for a in aircraft), default=None),
        "updated_at": state["updated_at"],
    })


async def api_search(request: web.Request) -> web.Response:
    query = (request.query.get("q") or "").strip()
    if not query:
        return web.json_response({"count": 0, "results": []})
    pattern = f"%{query}%"
    with dbconn() as conn:
        rows = [dict(row) for row in conn.execute(
            """SELECT * FROM seen_aircraft WHERE hex LIKE ? OR flight LIKE ? OR registration LIKE ?
            OR aircraft_type LIKE ? OR operator LIKE ? OR manufacturer LIKE ? OR model LIKE ? OR country LIKE ?
            ORDER BY last_seen DESC LIMIT 100""",
            (pattern.lower(), pattern.upper(), pattern.upper(), pattern.upper(), pattern, pattern, pattern, pattern))]
    return web.json_response({"count": len(rows), "results": rows})


async def api_aircraft_hex(request: web.Request) -> web.Response:
    hx = request.match_info["hex"].lower()
    with dbconn() as conn:
        summary = conn.execute("SELECT * FROM seen_aircraft WHERE hex=?", (hx,)).fetchone()
        track = [dict(row) for row in conn.execute(
            "SELECT ts,lat,lon,altitude_ft,speed_kt,track_deg,distance_km FROM observations WHERE hex=? ORDER BY ts DESC LIMIT 1000",
            (hx,))]
    if not summary:
        return web.json_response({"error": "not_found"}, status=404)
    return web.json_response({"aircraft": dict(summary), "track": track})


async def api_export_csv(request: web.Request) -> web.Response:
    hours = query_int(request, "hours", 24, 1, 24 * 365)
    since = int(time.time()) - hours * 3600
    with dbconn() as conn:
        rows = conn.execute("SELECT * FROM observations WHERE ts>=? ORDER BY ts DESC LIMIT 100000", (since,)).fetchall()
    output = io.StringIO()
    writer = csv.writer(output)
    if rows:
        writer.writerow(rows[0].keys())
        writer.writerows([tuple(row) for row in rows])
    return web.Response(text=output.getvalue(), content_type="text/csv",
                        headers={"Content-Disposition": f'attachment; filename="adsb-history-{hours}h.csv"'})


async def api_config(_: web.Request) -> web.Response:
    return web.json_response({**CFG, "version": VERSION})


async def ws_live(request: web.Request) -> web.WebSocketResponse:
    ws = web.WebSocketResponse(heartbeat=25)
    await ws.prepare(request)
    websockets.add(ws)
    await ws.send_json({"type": "aircraft", "data": snapshot()})
    async for msg in ws:
        if msg.type == WSMsgType.TEXT and msg.data == "ping":
            await ws.send_str("pong")
    websockets.discard(ws)
    return ws


async def root(_: web.Request) -> web.FileResponse:
    return web.FileResponse(BASE / "static" / "index.html")


async def health(_: web.Request) -> web.Response:
    return web.Response(text="ok\n" if state["source_ok"] else "degraded\n",
                        status=200 if state["source_ok"] else 503)


async def start_tasks(app: web.Application) -> None:
    load_aircraft_db()
    app["poller"] = asyncio.create_task(poller(app))
    app["historian"] = asyncio.create_task(historian(app))


async def stop_tasks(app: web.Application) -> None:
    for name in ("poller", "historian"):
        app[name].cancel()
        try:
            await app[name]
        except asyncio.CancelledError:
            pass


init_db()
app = web.Application(client_max_size=1024 * 1024)
app.on_startup.append(start_tasks)
app.on_cleanup.append(stop_tasks)
app.router.add_get("/", root)
app.router.add_static("/static", BASE / "static")
app.router.add_get("/health", health)
app.router.add_get("/api/status", api_status)
app.router.add_get("/api/aircraft", api_aircraft)
app.router.add_get("/api/nearest", api_nearest)
app.router.add_get("/api/receiver", api_receiver)
app.router.add_get("/api/system", api_system)
app.router.add_get("/api/history", api_history)
app.router.add_get("/api/history/stats", api_stats)
app.router.add_get("/api/statistics/live", api_live_stats)
app.router.add_get("/api/track/{hex}", api_track)
app.router.add_get("/api/search", api_search)
app.router.add_get("/api/aircraft/{hex}", api_aircraft_hex)
app.router.add_get("/api/export.csv", api_export_csv)
app.router.add_get("/api/config", api_config)
app.router.add_get("/live", ws_live)

if __name__ == "__main__":
    web.run_app(app, host=str(CFG["listen_host"]), port=int(CFG["listen_port"]), print=None)
