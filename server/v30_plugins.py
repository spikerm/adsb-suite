#!/usr/bin/env python3
"""ADS-B Suite V3 plugin registry and web manager."""
from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Any

from aiohttp import web

PLUGIN_STATE = Path(os.environ.get("ADSB_SUITE_PLUGIN_STATE", "/var/lib/adsb-suite/plugins.json"))

BUILTINS: list[dict[str, Any]] = [
    {"id":"core","name":"Core Radar","version":"3.0.0","category":"core","description":"Live readsb radar, historie en WebSocket-feed.","required":True,"default_enabled":True,"config":{}},
    {"id":"replay","name":"Flight Replay","version":"1.0.0","category":"analysis","description":"Historische vluchtweergave en tijdlijn.","default_enabled":True,"config":{"max_hours":24}},
    {"id":"aviation","name":"Aviation Layers","version":"1.1.0","category":"map","description":"Luchthavens, luchtruim, navigatiepunten en METAR/TAF.","default_enabled":True,"config":{"weather_enabled":True}},
    {"id":"coverage","name":"Coverage Analyzer","version":"1.2.0","category":"analysis","description":"Polar plot, heatmap en bereikstatistieken.","default_enabled":True,"config":{"default_hours":24}},
    {"id":"spotter","name":"Spotter Pro","version":"2.0.0","category":"alerts","description":"Spotterprofielen, browsermeldingen en noodsquawks.","default_enabled":True,"config":{"cooldown_seconds":300}},
    {"id":"homey","name":"Homey Webhook","version":"3.0.0","category":"integration","description":"Stuurt spotter- en systeemgebeurtenissen naar een Homey Logic/Webhook-flow.","default_enabled":False,"config":{"webhook_url":"","events":["spotter.alert","squawk.emergency"]}},
    {"id":"mqtt","name":"MQTT Bridge","version":"3.0.0","category":"integration","description":"Configuratiebasis voor publicatie van live vliegtuigen en alerts naar MQTT.","default_enabled":False,"config":{"host":"","port":1883,"username":"","password":"","base_topic":"adsb-suite","retain":False}},
    {"id":"acars","name":"ACARS","version":"0.1.0","category":"decoder","description":"Voorbereide plugin-slot voor ACARS-berichten.","default_enabled":False,"experimental":True,"config":{"source":""}},
    {"id":"vdl2","name":"VDL2","version":"0.1.0","category":"decoder","description":"Voorbereide plugin-slot voor VDL Mode 2.","default_enabled":False,"experimental":True,"config":{"source":""}},
    {"id":"ais","name":"AIS","version":"0.1.0","category":"decoder","description":"Voorbereide plugin-slot voor scheepvaart-AIS.","default_enabled":False,"experimental":True,"config":{"source":""}},
]


def _load() -> dict[str, Any]:
    state: dict[str, Any] = {"schema":1,"updated_at":0,"plugins":{}}
    try:
        if PLUGIN_STATE.exists():
            raw=json.loads(PLUGIN_STATE.read_text(encoding="utf-8"))
            if isinstance(raw,dict): state.update(raw)
    except (OSError,json.JSONDecodeError):
        pass
    state.setdefault("plugins",{})
    return state


def _save(state: dict[str, Any]) -> None:
    PLUGIN_STATE.parent.mkdir(parents=True,exist_ok=True)
    state["updated_at"]=int(time.time())
    tmp=PLUGIN_STATE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")
    os.replace(tmp,PLUGIN_STATE)


def _resolved() -> list[dict[str, Any]]:
    state=_load(); saved=state.get("plugins",{})
    result=[]
    for definition in BUILTINS:
        item=dict(definition)
        stored=saved.get(item["id"],{}) if isinstance(saved,dict) else {}
        enabled=True if item.get("required") else bool(stored.get("enabled",item.get("default_enabled",False)))
        config=dict(item.get("config",{})); config.update(stored.get("config",{}) if isinstance(stored,dict) else {})
        item.update(enabled=enabled,config=config,status="running" if enabled else "disabled")
        result.append(item)
    return result


def plugin_enabled(plugin_id: str) -> bool:
    return any(p["id"]==plugin_id and p["enabled"] for p in _resolved())


async def list_plugins(_: web.Request) -> web.Response:
    plugins=_resolved()
    return web.json_response({"version":"3.0.0","plugins":plugins,"enabled":sum(1 for p in plugins if p["enabled"]),"total":len(plugins)},headers={"Cache-Control":"no-store"})


async def set_enabled(request: web.Request) -> web.Response:
    pid=request.match_info["plugin_id"]
    definition=next((p for p in BUILTINS if p["id"]==pid),None)
    if not definition: raise web.HTTPNotFound(text="Onbekende plugin")
    if definition.get("required"): raise web.HTTPConflict(text="Core-plugin kan niet worden uitgeschakeld")
    data=await request.json()
    enabled=bool(data.get("enabled"))
    state=_load(); entry=state["plugins"].setdefault(pid,{})
    entry["enabled"]=enabled; entry.setdefault("config",{})
    _save(state)
    return web.json_response({"ok":True,"id":pid,"enabled":enabled,"restart_required":False})


async def update_config(request: web.Request) -> web.Response:
    pid=request.match_info["plugin_id"]
    definition=next((p for p in BUILTINS if p["id"]==pid),None)
    if not definition: raise web.HTTPNotFound(text="Onbekende plugin")
    data=await request.json(); config=data.get("config",data)
    if not isinstance(config,dict): raise web.HTTPBadRequest(text="config moet een object zijn")
    allowed=set(definition.get("config",{}))
    clean={k:v for k,v in config.items() if k in allowed}
    state=_load(); entry=state["plugins"].setdefault(pid,{})
    entry.setdefault("enabled",definition.get("default_enabled",False)); entry["config"]={**definition.get("config",{}),**clean}
    _save(state)
    return web.json_response({"ok":True,"id":pid,"config":entry["config"]})


async def health(_: web.Request) -> web.Response:
    plugins=_resolved()
    return web.json_response({"framework":"ok","version":"3.0.0","state_file":str(PLUGIN_STATE),"plugins":[{"id":p["id"],"enabled":p["enabled"],"status":p["status"]} for p in plugins]},headers={"Cache-Control":"no-store"})


def register_plugin_manager(app: web.Application) -> None:
    app.router.add_get("/api/v3/plugins",list_plugins)
    app.router.add_put("/api/v3/plugins/{plugin_id}/enabled",set_enabled)
    app.router.add_put("/api/v3/plugins/{plugin_id}/config",update_config)
    app.router.add_get("/api/v3/plugins/health",health)
