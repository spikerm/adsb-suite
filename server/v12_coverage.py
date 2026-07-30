#!/usr/bin/env python3
import csv, io, json, math, sqlite3, time
from pathlib import Path
from aiohttp import web

def register_coverage(app, db_path):
    db_path=Path(db_path)
    def conn():
        c=sqlite3.connect(db_path,timeout=20);c.row_factory=sqlite3.Row;c.execute('PRAGMA busy_timeout=5000');return c
    def period(req):
        hours=max(1,min(24*365,int(req.query.get('hours','24'))));return int(time.time())-hours*3600,hours
    def load(req):
        since,hours=period(req)
        with conn() as c: rows=[dict(r) for r in c.execute('SELECT ts,hex,flight,registration,lat,lon,altitude_ft,distance_km,bearing_deg,rssi FROM observations WHERE ts>=? AND lat IS NOT NULL AND lon IS NOT NULL AND distance_km IS NOT NULL',(since,))]
        return rows,hours
    async def summary(req):
        rows,hours=load(req);dist=sorted(float(r['distance_km']) for r in rows);n=len(dist)
        pct=lambda p: dist[min(n-1,max(0,round((n-1)*p)))] if n else None
        sectors=[]
        for start in range(0,360,10):
            vals=[float(r['distance_km']) for r in rows if r['bearing_deg'] is not None and start<=float(r['bearing_deg'])<start+10]
            sectors.append({'bearing':start+5,'max_km':round(max(vals),1) if vals else 0,'p95_km':round(sorted(vals)[int((len(vals)-1)*.95)],1) if vals else 0,'count':len(vals)})
        bands=[(0,1000),(1000,5000),(5000,10000),(10000,20000),(20000,30000),(30000,999999)]
        altitude=[]
        for lo,hi in bands:
            vals=[float(r['distance_km']) for r in rows if r['altitude_ft'] is not None and lo<=float(r['altitude_ft'])<hi]
            altitude.append({'label':f'{lo//1000}–{hi//1000}k ft' if hi<999999 else '30k+ ft','count':len(vals),'max_km':round(max(vals),1) if vals else 0,'avg_km':round(sum(vals)/len(vals),1) if vals else 0})
        cells={}
        for r in rows:
            key=f"{round(float(r['lat']),2):.2f},{round(float(r['lon']),2):.2f}";x=cells.setdefault(key,{'lat':round(float(r['lat']),2),'lon':round(float(r['lon']),2),'count':0,'max_km':0});x['count']+=1;x['max_km']=max(x['max_km'],round(float(r['distance_km']),1))
        far=max(rows,key=lambda r:float(r['distance_km'])) if rows else None
        return web.json_response({'hours':hours,'observations':n,'unique_aircraft':len({r['hex'] for r in rows}),'max_km':round(max(dist),1) if n else None,'avg_km':round(sum(dist)/n,1) if n else None,'median_km':round(pct(.5),1) if n else None,'p95_km':round(pct(.95),1) if n else None,'farthest':far,'polar':sectors,'altitude_bands':altitude,'heatmap':list(cells.values())},headers={'Cache-Control':'no-store'})
    async def export(req):
        rows,hours=load(req);fmt=req.match_info['fmt']
        if fmt=='geojson':
            data={'type':'FeatureCollection','features':[{'type':'Feature','geometry':{'type':'Point','coordinates':[r['lon'],r['lat']]},'properties':{k:v for k,v in r.items() if k not in ('lat','lon')}} for r in rows]};return web.json_response(data,headers={'Content-Disposition':f'attachment; filename="coverage-{hours}h.geojson"'})
        out=io.StringIO();w=csv.DictWriter(out,fieldnames=list(rows[0].keys()) if rows else ['ts','hex','lat','lon','distance_km']);w.writeheader();w.writerows(rows)
        return web.Response(text=out.getvalue(),content_type='text/csv',headers={'Content-Disposition':f'attachment; filename="coverage-{hours}h.csv"'})
    app.router.add_get('/api/coverage',summary);app.router.add_get('/api/coverage/export.{fmt}',export)
