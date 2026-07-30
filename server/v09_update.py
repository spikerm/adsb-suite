#!/usr/bin/env python3
import asyncio
import json
import time
import urllib.request
from pathlib import Path
from aiohttp import web
from v10_replay import register_replay

CACHE = {'ts': 0.0, 'data': None}


def _version_tuple(value):
    parts = []
    for part in str(value or '').strip().lstrip('v').split('.'):
        digits = ''.join(ch for ch in part if ch.isdigit())
        parts.append(int(digits or 0))
    return tuple((parts + [0, 0, 0])[:3])


def _fetch_latest(ref):
    url = f'https://raw.githubusercontent.com/spikerm/adsb-suite/{ref}/VERSION'
    request = urllib.request.Request(url, headers={'User-Agent': 'ADS-B-Suite'})
    with urllib.request.urlopen(request, timeout=8) as response:
        return response.read().decode('utf-8').strip()


def register_update_api(app, version, cfg):
    async def update_status(request):
        ref = str(cfg.get('update_channel') or 'feature/v0.9-smart-dashboard')
        now = time.time()
        if CACHE['data'] is not None and now - CACHE['ts'] < 900 and request.query.get('refresh') != '1':
            return web.json_response(CACHE['data'])
        try:
            latest = await asyncio.to_thread(_fetch_latest, ref)
            data = {
                'ok': True,
                'current_version': str(version),
                'latest_version': latest,
                'update_available': _version_tuple(latest) > _version_tuple(version),
                'channel': ref,
                'checked_at': int(now),
            }
        except Exception as exc:
            data = {
                'ok': False,
                'current_version': str(version),
                'latest_version': None,
                'update_available': False,
                'channel': ref,
                'checked_at': int(now),
                'error': f'{type(exc).__name__}: {exc}',
            }
        CACHE.update(ts=now, data=data)
        return web.json_response(data, headers={'Cache-Control': 'no-store'})

    app.router.add_get('/api/update-status', update_status)
    register_replay(app, Path(str(cfg.get('database_path') or '/var/lib/adsb-suite/adsb-suite.db')))
