#!/usr/bin/env python3
"""Flight Replay API for ADS-B Suite."""
import sqlite3
import time
from pathlib import Path
from aiohttp import web


def _int_arg(request, name, default, minimum, maximum):
    try:
        value = int(request.query.get(name, default))
    except (TypeError, ValueError):
        raise web.HTTPBadRequest(text=f'Ongeldige parameter: {name}')
    return max(minimum, min(maximum, value))


def register_replay(app, db_path):
    db_path = Path(db_path)

    def connect():
        conn = sqlite3.connect(db_path, timeout=20)
        conn.row_factory = sqlite3.Row
        conn.execute('PRAGMA busy_timeout=5000')
        return conn

    async def replay_range(_request):
        with connect() as conn:
            row = conn.execute('SELECT MIN(ts) first_ts, MAX(ts) last_ts, COUNT(*) observations FROM observations').fetchone()
        now = int(time.time())
        return web.json_response({
            'first_ts': row['first_ts'], 'last_ts': row['last_ts'],
            'observations': row['observations'], 'server_ts': now,
            'max_window_seconds': 86400,
        }, headers={'Cache-Control': 'no-store'})

    async def replay_data(request):
        now = int(time.time())
        end = _int_arg(request, 'end', now, 1, now + 300)
        start = _int_arg(request, 'start', end - 3600, 1, end)
        step = _int_arg(request, 'step', 10, 5, 300)
        if end - start > 86400:
            raise web.HTTPBadRequest(text='Een replayvenster mag maximaal 24 uur zijn')
        max_rows = 150000
        sql = '''
        WITH ranked AS (
          SELECT
            (ts / ?) * ? bucket, ts, hex, flight, registration, aircraft_type,
            description, operator, lat, lon, altitude_ft, speed_kt, track_deg,
            vertical_rate_fpm, distance_km, bearing_deg, squawk, emergency,
            category, rssi,
            ROW_NUMBER() OVER (PARTITION BY hex, (ts / ?) ORDER BY ts DESC) rn
          FROM observations
          WHERE ts BETWEEN ? AND ? AND lat IS NOT NULL AND lon IS NOT NULL
        )
        SELECT * FROM ranked WHERE rn=1 ORDER BY bucket, hex LIMIT ?
        '''
        with connect() as conn:
            rows = [dict(r) for r in conn.execute(sql, (step, step, step, start, end, max_rows + 1))]
        truncated = len(rows) > max_rows
        if truncated:
            rows = rows[:max_rows]
        frames = {}
        for row in rows:
            bucket = str(row.pop('bucket'))
            row.pop('rn', None)
            frames.setdefault(bucket, []).append(row)
        return web.json_response({
            'start': start, 'end': end, 'step': step,
            'frame_count': len(frames), 'observation_count': len(rows),
            'truncated': truncated, 'frames': frames,
        }, headers={'Cache-Control': 'no-store'})

    app.router.add_get('/api/replay/range', replay_range)
    app.router.add_get('/api/replay', replay_data)
