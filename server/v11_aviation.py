#!/usr/bin/env python3
import asyncio, json, time, urllib.parse, urllib.request
from pathlib import Path
from aiohttp import web

WEATHER_CACHE = {}
DEFAULT_AIRPORTS = [
 {'icao':'EHAM','iata':'AMS','name':'Amsterdam Airport Schiphol','lat':52.3086,'lon':4.7639,'type':'airport'},
 {'icao':'EHRD','iata':'RTM','name':'Rotterdam The Hague Airport','lat':51.9569,'lon':4.4372,'type':'airport'},
 {'icao':'EHEH','iata':'EIN','name':'Eindhoven Airport','lat':51.4501,'lon':5.3745,'type':'airport'},
 {'icao':'EHWO','iata':'WOE','name':'Woensdrecht Air Base','lat':51.4491,'lon':4.3420,'type':'military'},
 {'icao':'EHKD','iata':'DHR','name':'De Kooy Airfield','lat':52.9234,'lon':4.7806,'type':'military'},
 {'icao':'EHLW','iata':'LWR','name':'Leeuwarden Air Base','lat':53.2286,'lon':5.7606,'type':'military'},
 {'icao':'EHTW','iata':'ENS','name':'Twente Airport','lat':52.2758,'lon':6.8892,'type':'airport'},
 {'icao':'EHGG','iata':'GRQ','name':'Groningen Airport Eelde','lat':53.1197,'lon':6.5794,'type':'airport'},
 {'icao':'EHBK','iata':'MST','name':'Maastricht Aachen Airport','lat':50.9117,'lon':5.7701,'type':'airport'},
 {'icao':'EHLE','iata':'LEY','name':'Lelystad Airport','lat':52.4603,'lon':5.5272,'type':'airport'},
 {'icao':'EHTE','iata':'','name':'Teuge Airport','lat':52.2447,'lon':6.0467,'type':'airport'},
 {'icao':'EHMZ','iata':'','name':'Midden-Zeeland Airport','lat':51.5122,'lon':3.7311,'type':'airport'}
]

def _feature(point):
    return {'type':'Feature','geometry':{'type':'Point','coordinates':[point['lon'],point['lat']]},'properties':{k:v for k,v in point.items() if k not in ('lat','lon')}}

def _geojson(points): return {'type':'FeatureCollection','features':[_feature(p) for p in points]}

def _read_geojson(path):
    try:
        data=json.loads(path.read_text(encoding='utf-8'))
        return data if data.get('type') in ('FeatureCollection','Feature') else {'type':'FeatureCollection','features':[]}
    except Exception:
        return {'type':'FeatureCollection','features':[]}

def _weather(icao):
    q=urllib.parse.urlencode({'ids':icao,'format':'json'})
    headers={'User-Agent':'ADS-B-Suite/1.1'}
    result={}
    for kind in ('metar','taf'):
        req=urllib.request.Request(f'https://aviationweather.gov/api/data/{kind}?{q}',headers=headers)
        try:
            with urllib.request.urlopen(req,timeout=8) as response:
                result[kind]=json.loads(response.read().decode('utf-8')) if response.status != 204 else []
        except Exception as exc:
            result[kind]=[]; result[f'{kind}_error']=f'{type(exc).__name__}: {exc}'
    return result

def register_aviation_api(app, base, cfg):
    data_dir=Path(str(cfg.get('aviation_data_dir') or '/var/lib/adsb-suite/aviation'))
    async def index(_):
        layers=['airports','navaids','waypoints','ctr','tma','fir','restricted','danger','prohibited']
        return web.json_response({'layers':layers,'weather':True,'data_dir':str(data_dir)})
    async def layer(request):
        name=request.match_info['name'].lower()
        if name=='airports': data=_geojson(DEFAULT_AIRPORTS)
        else: data=_read_geojson(data_dir/f'{name}.geojson')
        return web.json_response(data,headers={'Cache-Control':'public, max-age=300'})
    async def weather(request):
        icao=request.match_info['icao'].upper()
        if len(icao)!=4 or not icao.isalnum(): raise web.HTTPBadRequest(text='Ongeldige ICAO-code')
        now=time.time(); cached=WEATHER_CACHE.get(icao)
        if cached and now-cached['ts']<300: return web.json_response(cached['data'])
        data=await asyncio.to_thread(_weather,icao); data.update({'icao':icao,'checked_at':int(now)})
        WEATHER_CACHE[icao]={'ts':now,'data':data}
        return web.json_response(data,headers={'Cache-Control':'no-store'})
    app.router.add_get('/api/aviation',index)
    app.router.add_get('/api/aviation/layer/{name}',layer)
    app.router.add_get('/api/aviation/weather/{icao}',weather)
