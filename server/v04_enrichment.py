"""ADS-B Suite v0.4 external enrichment with conservative caching."""
from __future__ import annotations

import asyncio
import time
from typing import Any

from aiohttp import ClientSession, ClientTimeout

_CACHE: dict[str, tuple[float, dict[str, Any]]] = {}
_TTL = 6 * 3600


def _clean_flight(value: Any) -> str:
    return str(value or "").strip().upper().replace(" ", "")


async def _get_json(url: str) -> dict[str, Any] | None:
    timeout = ClientTimeout(total=8, connect=4)
    headers = {"User-Agent": "ADS-B-Suite/0.4 (+local receiver dashboard)"}
    try:
        async with ClientSession(timeout=timeout, headers=headers) as session:
            async with session.get(url) as response:
                if response.status != 200:
                    return None
                data = await response.json(content_type=None)
                return data if isinstance(data, dict) else None
    except (asyncio.TimeoutError, OSError, ValueError):
        return None


def _photo(data: dict[str, Any] | None) -> dict[str, Any] | None:
    photos = (data or {}).get("photos")
    if not isinstance(photos, list) or not photos:
        return None
    photo = photos[0] if isinstance(photos[0], dict) else {}
    thumb = photo.get("thumbnail_large") or photo.get("thumbnail") or {}
    image = thumb.get("src") if isinstance(thumb, dict) else None
    if not image:
        return None
    return {
        "image": image,
        "link": photo.get("link"),
        "photographer": photo.get("photographer"),
    }


def _route(data: dict[str, Any] | None) -> dict[str, Any] | None:
    response = (data or {}).get("response")
    if not isinstance(response, dict):
        return None
    flight = response.get("flightroute")
    if not isinstance(flight, dict):
        return None
    origin = flight.get("origin") if isinstance(flight.get("origin"), dict) else {}
    destination = flight.get("destination") if isinstance(flight.get("destination"), dict) else {}
    airline = flight.get("airline") if isinstance(flight.get("airline"), dict) else {}
    return {
        "callsign": flight.get("callsign"),
        "number": flight.get("callsign_icao") or flight.get("callsign_iata"),
        "origin": {
            "icao": origin.get("icao_code"),
            "iata": origin.get("iata_code"),
            "name": origin.get("name"),
            "municipality": origin.get("municipality"),
            "country": origin.get("country_name"),
        },
        "destination": {
            "icao": destination.get("icao_code"),
            "iata": destination.get("iata_code"),
            "name": destination.get("name"),
            "municipality": destination.get("municipality"),
            "country": destination.get("country_name"),
        },
        "airline": {
            "name": airline.get("name"),
            "icao": airline.get("icao"),
            "iata": airline.get("iata"),
            "country": airline.get("country"),
        },
    }


async def enrich_aircraft(aircraft: dict[str, Any]) -> dict[str, Any]:
    hx = str(aircraft.get("hex") or "").strip().lower()
    flight = _clean_flight(aircraft.get("flight"))
    key = f"{hx}:{flight}"
    cached = _CACHE.get(key)
    if cached and cached[0] > time.time():
        return cached[1]

    photo_url = f"https://api.planespotters.net/pub/photos/hex/{hx}" if hx else ""
    route_url = f"https://api.adsbdb.com/v0/callsign/{flight}" if flight else ""
    photo_data, route_data = await asyncio.gather(
        _get_json(photo_url) if photo_url else asyncio.sleep(0, result=None),
        _get_json(route_url) if route_url else asyncio.sleep(0, result=None),
    )
    result = {
        "hex": hx,
        "flight": flight or None,
        "photo": _photo(photo_data),
        "route": _route(route_data),
        "updated_at": int(time.time()),
    }
    _CACHE[key] = (time.time() + _TTL, result)
    return result
