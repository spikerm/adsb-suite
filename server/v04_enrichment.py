"""ADS-B Suite external route and aircraft-photo enrichment with caching."""
from __future__ import annotations

import asyncio
import time
from typing import Any
from urllib.parse import quote

from aiohttp import ClientSession, ClientTimeout

_CACHE: dict[str, tuple[float, dict[str, Any]]] = {}
_TTL = 6 * 3600


def _clean_flight(value: Any) -> str:
    return str(value or "").strip().upper().replace(" ", "")


async def _get_json(url: str) -> dict[str, Any] | None:
    timeout = ClientTimeout(total=10, connect=4)
    headers = {"User-Agent": "ADS-B-Suite/0.6 (+local receiver dashboard)", "Accept": "application/json"}
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
    for candidate in photos:
        if not isinstance(candidate, dict):
            continue
        image = None
        for key in ("thumbnail_large", "thumbnail", "thumbnail_small"):
            thumb = candidate.get(key)
            if isinstance(thumb, dict) and thumb.get("src"):
                image = thumb["src"]
                break
            if isinstance(thumb, str) and thumb:
                image = thumb
                break
        image = image or candidate.get("image") or candidate.get("src")
        if image:
            return {
                "image": image,
                "link": candidate.get("link") or candidate.get("url"),
                "photographer": candidate.get("photographer"),
            }
    return None


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
        "origin": {"icao": origin.get("icao_code"), "iata": origin.get("iata_code"), "name": origin.get("name"), "municipality": origin.get("municipality"), "country": origin.get("country_name")},
        "destination": {"icao": destination.get("icao_code"), "iata": destination.get("iata_code"), "name": destination.get("name"), "municipality": destination.get("municipality"), "country": destination.get("country_name")},
        "airline": {"name": airline.get("name"), "icao": airline.get("icao"), "iata": airline.get("iata"), "country": airline.get("country")},
    }


async def enrich_aircraft(aircraft: dict[str, Any]) -> dict[str, Any]:
    hx = str(aircraft.get("hex") or "").strip().lower()
    registration = str(aircraft.get("registration") or aircraft.get("r") or "").strip().upper()
    flight = _clean_flight(aircraft.get("flight"))
    key = f"{hx}:{registration}:{flight}"
    cached = _CACHE.get(key)
    if cached and cached[0] > time.time():
        return cached[1]

    route_url = f"https://api.adsbdb.com/v0/callsign/{quote(flight)}" if flight else ""
    hex_url = f"https://api.planespotters.net/pub/photos/hex/{quote(hx)}" if hx else ""
    photo_data, route_data = await asyncio.gather(
        _get_json(hex_url) if hex_url else asyncio.sleep(0, result=None),
        _get_json(route_url) if route_url else asyncio.sleep(0, result=None),
    )
    photo = _photo(photo_data)
    if photo is None and registration:
        photo = _photo(await _get_json(f"https://api.planespotters.net/pub/photos/reg/{quote(registration)}"))

    result = {"hex": hx, "flight": flight or None, "registration": registration or None, "photo": photo, "route": _route(route_data), "updated_at": int(time.time())}
    _CACHE[key] = (time.time() + _TTL, result)
    return result
