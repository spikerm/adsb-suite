#!/usr/bin/env python3
import asyncio, hashlib, hmac, json, os, secrets, shutil, sqlite3, subprocess, time
from pathlib import Path
from aiohttp import web

SESSIONS = {}
ALLOWED_CONFIG = {
    'receiver_name': str, 'receiver_lat': float, 'receiver_lon': float, 'antenna_height_m': float,
    'poll_interval_seconds': float, 'history_interval_seconds': int,
    'retention_days': int, 'track_default_minutes': int, 'track_max_hours': int,
}
SETUP_CONFIG = {**ALLOWED_CONFIG, 'source_file': str}

def _hash_password(password, salt=None):
    salt = salt or secrets.token_hex(16)
    digest = hashlib.pbkdf2_hmac('sha256', password.encode(), salt.encode(), 240000).hex()
    return f'pbkdf2_sha256${salt}${digest}'

def _verify_password(password, stored):
    try:
        _, salt, expected = stored.split('$', 2)
        actual = _hash_password(password, salt).split('$', 2)[2]
        return hmac.compare_digest(actual, expected)
    except Exception:
        return False

def _session(request):
    token = request.cookies.get('adsb_admin_session', '')
    data = SESSIONS.get(token)
    if not data or data < time.time():
        SESSIONS.pop(token, None)
        return False
    return True

def _require(request):
    if not _session(request): raise web.HTTPUnauthorized(text='Niet ingelogd')

def _run_helper(action, *args, timeout=45):
    cmd = ['sudo', '-n', '/usr/local/sbin/adsb-suite-admin-helper', action, *map(str, args)]
    p = subprocess.run(cmd, text=True, capture_output=True, timeout=timeout)
    return {'ok': p.returncode == 0, 'code': p.returncode, 'output': (p.stdout + p.stderr)[-30000:]}

def _write_config(config_path, current):
    config_path.write_text(json.dumps(current, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

def _service_exists(name):
    try:
        p = subprocess.run(['systemctl', 'list-unit-files', f'{name}.service', '--no-legend'], text=True, capture_output=True, timeout=5)
        return p.returncode == 0 and f'{name}.service' in p.stdout
    except Exception:
        return False

def _service_active(name):
    try:
        p = subprocess.run(['systemctl', 'is-active', name], text=True, capture_output=True, timeout=5)
        return p.stdout.strip() == 'active'
    except Exception:
        return False

def _safe_exists(path):
    try: return Path(path).exists()
    except (OSError, PermissionError): return False

def register_admin(app, base, cfg, config_path, db_path, state, version):
    base, config_path, db_path = Path(base), Path(config_path), Path(db_path)
    async def admin_page(_): return web.FileResponse(base / 'static' / 'admin.html')
    async def setup_page(_): return web.FileResponse(base / 'static' / 'setup.html')

    async def login(request):
        body = await request.json(); password = str(body.get('password') or '')
        if not _verify_password(password, str(cfg.get('admin_password_hash') or '')):
            await asyncio.sleep(0.4); return web.json_response({'ok': False, 'error': 'Onjuist wachtwoord'}, status=401)
        token = secrets.token_urlsafe(32); SESSIONS[token] = time.time() + 8 * 3600
        response = web.json_response({'ok': True, 'setup_complete': bool(cfg.get('setup_complete'))})
        response.set_cookie('adsb_admin_session', token, httponly=True, samesite='Strict', max_age=28800)
        return response

    async def logout(request):
        SESSIONS.pop(request.cookies.get('adsb_admin_session', ''), None)
        response = web.json_response({'ok': True}); response.del_cookie('adsb_admin_session'); return response

    async def me(request): return web.json_response({'authenticated': _session(request), 'version': version, 'setup_complete': bool(cfg.get('setup_complete'))})

    async def diagnostics(request):
        try:
            source = Path(str(cfg.get('source_file', '/run/readsb/aircraft.json')))
            try:
                source_ok = source.exists() and time.time() - source.stat().st_mtime < 30
            except OSError:
                source_ok = False
            try:
                disk = shutil.disk_usage('/')
                disk_ok = disk.free > 512 * 1024 * 1024
                disk_detail = f'{disk.free / 1073741824:.1f} GB vrij'
            except OSError as exc:
                disk_ok, disk_detail = False, str(exc)
            suite_active = await asyncio.to_thread(_service_active, 'adsb-suite')
            readsb_active = await asyncio.to_thread(_service_active, 'readsb')
            checks = [
                {'name':'ADS-B Suite service','ok':suite_active,'detail':'active' if suite_active else 'niet actief'},
                {'name':'readsb service','ok':readsb_active,'detail':'active' if readsb_active else 'niet actief'},
                {'name':'Live bronbestand','ok':source_ok,'detail':str(source)},
                {'name':'Vliegtuigdatabase','ok':_safe_exists('/usr/local/share/tar1090/aircraft.csv.gz'),'detail':'tar1090 database'},
                {'name':'HTTPS-certificaat','ok':_safe_exists('/var/lib/adsb-suite/public/adsb-suite-ca.crt'),'detail':'poort 8443'},
                {'name':'Schijfruimte','ok':disk_ok,'detail':disk_detail},
            ]
            failures = sum(not c['ok'] for c in checks)
            return web.json_response({'overall':'ok' if failures == 0 else 'warning' if failures <= 2 else 'error','checks':checks,'timestamp':int(time.time())})
        except Exception as exc:
            return web.json_response({'overall':'error','checks':[{'name':'Diagnose','ok':False,'detail':f'{type(exc).__name__}: {exc}'}],'timestamp':int(time.time())}, status=200)

    async def overview(request):
        _require(request); disk = shutil.disk_usage('/'); db_size = db_path.stat().st_size if db_path.exists() else 0
        source = Path(str(cfg.get('source_file', '/run/readsb/aircraft.json'))); services = {}
        for service in ('adsb-suite', 'readsb', 'tar1090'):
            r = await asyncio.to_thread(_run_helper, 'service-status', service); services[service] = r['output'].strip()
        temp = None
        try: temp = int(Path('/sys/class/thermal/thermal_zone0/temp').read_text()) / 1000
        except Exception: pass
        return web.json_response({'version': version, 'services': services, 'source_exists': source.exists(),
            'source_age_seconds': round(time.time()-source.stat().st_mtime,1) if source.exists() else None,
            'live_aircraft': len(state.get('aircraft', [])), 'messages_per_second': state.get('messages_per_second',0),
            'cpu_temperature_c': temp, 'load': os.getloadavg(), 'disk_total': disk.total, 'disk_used': disk.used,
            'disk_free': disk.free, 'database_size': db_size, 'setup_complete': bool(cfg.get('setup_complete'))})

    async def get_setup(request):
        _require(request); source = Path(str(cfg.get('source_file', '/run/readsb/aircraft.json')))
        checks = {
            'ADS-B Suite service': {'ok': _service_exists('adsb-suite')}, 'readsb service': {'ok': _service_exists('readsb')},
            'tar1090 service': {'ok': _service_exists('tar1090')}, 'Live bronbestand': {'ok': _safe_exists(source), 'detail': str(source)},
            'Vliegtuigdatabase': {'ok': _safe_exists('/usr/local/share/tar1090/aircraft.csv.gz')},
            'HTTPS': {'ok': _safe_exists('/var/lib/adsb-suite/public/adsb-suite-ca.crt'), 'detail':'https://<ip>:8443'},
        }
        return web.json_response({'complete': bool(cfg.get('setup_complete')), 'checks': checks, 'config': {k: cfg.get(k) for k in SETUP_CONFIG}})

    async def save_setup(request):
        _require(request); body = await request.json(); current = json.loads(config_path.read_text(encoding='utf-8'))
        for key, caster in SETUP_CONFIG.items():
            if key in body:
                value = caster(body[key])
                if key == 'receiver_lat' and not -90 <= value <= 90: raise web.HTTPBadRequest(text='Ongeldige latitude')
                if key == 'receiver_lon' and not -180 <= value <= 180: raise web.HTTPBadRequest(text='Ongeldige longitude')
                if key == 'antenna_height_m' and not -100 <= value <= 1000: raise web.HTTPBadRequest(text='Ongeldige antennehoogte')
                if key == 'source_file' and not value.startswith('/'): raise web.HTTPBadRequest(text='Bronbestand moet een absoluut pad zijn')
                current[key] = value
        current['setup_complete'] = True; _write_config(config_path, current); cfg.update(current)
        asyncio.create_task(asyncio.to_thread(_run_helper, 'restart-suite'))
        return web.json_response({'ok': True, 'restart_required': True})

    async def get_config(request): _require(request); return web.json_response({k: cfg.get(k) for k in ALLOWED_CONFIG})
    async def save_config(request):
        _require(request); body = await request.json(); current = json.loads(config_path.read_text(encoding='utf-8'))
        for key, caster in ALLOWED_CONFIG.items():
            if key in body:
                value = caster(body[key])
                if key == 'receiver_lat' and not -90 <= value <= 90: raise web.HTTPBadRequest(text='Ongeldige latitude')
                if key == 'receiver_lon' and not -180 <= value <= 180: raise web.HTTPBadRequest(text='Ongeldige longitude')
                if key == 'antenna_height_m' and not -100 <= value <= 1000: raise web.HTTPBadRequest(text='Ongeldige antennehoogte')
                current[key] = value
        _write_config(config_path, current); cfg.update(current); return web.json_response({'ok': True, 'restart_required': True})

    async def change_password(request):
        _require(request); body = await request.json(); new = str(body.get('password') or '')
        if len(new) < 10: raise web.HTTPBadRequest(text='Gebruik minimaal 10 tekens')
        current = json.loads(config_path.read_text(encoding='utf-8')); current['admin_password_hash'] = _hash_password(new)
        _write_config(config_path, current); cfg.update(current); return web.json_response({'ok': True})

    async def action(request):
        _require(request); name = request.match_info['name']; allowed = {'restart-suite','restart-readsb','update-database','update-suite','reboot'}
        if name not in allowed: raise web.HTTPBadRequest(text='Onbekende actie')
        result = await asyncio.to_thread(_run_helper, name, timeout=180); return web.json_response(result, status=200 if result['ok'] else 500)

    async def logs(request):
        _require(request); service = request.query.get('service','adsb-suite')
        if service not in ('adsb-suite','readsb','tar1090'): raise web.HTTPBadRequest(text='Ongeldige service')
        return web.json_response(await asyncio.to_thread(_run_helper, 'logs', service))

    async def backup(request):
        _require(request); result = await asyncio.to_thread(_run_helper, 'backup')
        if not result['ok']: return web.json_response(result, status=500)
        path = Path(result['output'].strip().splitlines()[-1]); return web.FileResponse(path, headers={'Content-Disposition': f'attachment; filename="{path.name}"'})

    async def cleanup(request):
        _require(request); body = await request.json(); days = max(1,min(3650,int(body.get('days',cfg.get('retention_days',90)))))
        cutoff = int(time.time())-days*86400
        with sqlite3.connect(db_path) as conn:
            deleted = conn.execute('DELETE FROM observations WHERE ts < ?', (cutoff,)).rowcount; conn.execute('VACUUM')
        return web.json_response({'ok': True, 'deleted': deleted, 'days': days})

    app.router.add_get('/admin', admin_page); app.router.add_get('/admin/', admin_page)
    app.router.add_get('/setup', setup_page); app.router.add_get('/setup/', setup_page)
    app.router.add_get('/api/diagnostics', diagnostics)
    app.router.add_post('/api/admin/login', login); app.router.add_post('/api/admin/logout', logout)
    app.router.add_get('/api/admin/me', me); app.router.add_get('/api/admin/overview', overview)
    app.router.add_get('/api/admin/setup', get_setup); app.router.add_put('/api/admin/setup', save_setup)
    app.router.add_get('/api/admin/config', get_config); app.router.add_put('/api/admin/config', save_config)
    app.router.add_put('/api/admin/password', change_password); app.router.add_post('/api/admin/action/{name}', action)
    app.router.add_get('/api/admin/logs', logs); app.router.add_get('/api/admin/backup', backup)
    app.router.add_post('/api/admin/cleanup', cleanup)
