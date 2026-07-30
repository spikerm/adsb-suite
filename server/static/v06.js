/* ADS-B Suite v0.8: playback, live selected-aircraft halo, auto-follow and day/night overlay. */
(() => {
  let selectionHalo = null;
  let playbackMarker = null;
  let playbackTimer = null;
  let nightLayer = null;
  let autoFollow = localStorage.getItem('adsbAutoFollow') === '1';
  let selectedUpdatedAt = 0;

  function stopPlayback() {
    if (playbackTimer) clearInterval(playbackTimer);
    playbackTimer = null;
    if (playbackMarker) map.removeLayer(playbackMarker);
    playbackMarker = null;
    const button = document.getElementById('playTrack');
    if (button) button.textContent = '▶ Track afspelen';
  }

  function updateNightLayer() {
    if (nightLayer) map.removeLayer(nightLayer);
    const now = new Date();
    const start = Date.UTC(now.getUTCFullYear(), 0, 0);
    const day = (now.getTime() - start) / 86400000;
    const decl = -23.44 * Math.cos((2 * Math.PI / 365) * (day + 10));
    const utcHours = now.getUTCHours() + now.getUTCMinutes() / 60;
    const subsolarLon = 180 - utcHours * 15;
    const points = [[90, -180], [90, 180]];
    for (let lon = 180; lon >= -180; lon -= 2) {
      const hourAngle = (lon - subsolarLon) * Math.PI / 180;
      const dec = decl * Math.PI / 180;
      const lat = Math.atan(-Math.cos(hourAngle) / Math.tan(dec || 0.00001)) * 180 / Math.PI;
      points.push([Math.max(-89.9, Math.min(89.9, lat)), lon]);
    }
    points.push([90, -180]);
    nightLayer = L.polygon(points, {stroke:false, fillColor:'#071020', fillOpacity:.25, interactive:false}).addTo(map);
    nightLayer.bringToBack();
  }

  async function playTrack(hex, minutes) {
    stopPlayback();
    const response = await fetch(`/api/track/${encodeURIComponent(hex)}?minutes=${minutes}`);
    if (!response.ok) return;
    const data = await response.json();
    const points = (data.points || []).filter(p => Number.isFinite(Number(p.lat)) && Number.isFinite(Number(p.lon)));
    if (points.length < 2) return;
    let i = 0;
    playbackMarker = L.circleMarker([points[0].lat, points[0].lon], {radius:7, weight:3, color:'#fff', fillColor:'#37ed9b', fillOpacity:1}).addTo(map).bindTooltip('Track playback', {permanent:true});
    const button = document.getElementById('playTrack');
    if (button) button.textContent = '■ Stop playback';
    playbackTimer = setInterval(() => {
      i += 1;
      if (i >= points.length) return stopPlayback();
      playbackMarker.setLatLng([points[i].lat, points[i].lon]);
    }, Math.max(120, Math.min(800, 12000 / points.length)));
  }

  function setDetailValue(labelText, value) {
    const detail = document.getElementById('detail');
    if (!detail) return;
    for (const dt of detail.querySelectorAll('dt')) {
      if (dt.textContent.trim() === labelText) {
        const dd = dt.nextElementSibling;
        if (dd) dd.textContent = value;
        break;
      }
    }
  }

  function updateLiveDetail(a) {
    const detail = document.getElementById('detail');
    if (!detail || !a || a.hex !== selectedHex) return;
    selectedUpdatedAt = Date.now();
    const title = detail.querySelector('h2');
    if (title) title.textContent = label(a);
    setDetailValue('Hoogte barometrisch', `${fmt(first(a.altitude_ft,a.alt_baro),0)} ft`);
    setDetailValue('Hoogte WGS84', `${fmt(a.alt_geom,0)} ft`);
    setDetailValue('Grondsnelheid', `${fmt(first(a.speed_kt,a.gs))} kt`);
    setDetailValue('Ware snelheid', `${fmt(a.tas)} kt`);
    setDetailValue('Aangegeven snelheid', `${fmt(a.ias)} kt`);
    setDetailValue('Mach', fmt(a.mach,3));
    setDetailValue('Afstand', `${fmt(a.distance_km)} km`);
    setDetailValue('Koers over grond', `${fmt(first(a.track_deg,a.track),1)}°`);
    setDetailValue('Ware neusrichting', `${fmt(a.true_heading,1)}°`);
    setDetailValue('Magnetische neusrichting', `${fmt(a.mag_heading,1)}°`);
    setDetailValue('Verticale snelheid baro', `${fmt(first(a.vertical_rate_fpm,a.baro_rate),0)} ft/min`);
    setDetailValue('Verticale snelheid geom.', `${fmt(a.geom_rate,0)} ft/min`);
    setDetailValue('RSSI', `${fmt(a.rssi)} dBFS`);
    setDetailValue('Berichten', fmt(a.messages,0));
    setDetailValue('Laatste positie', `${fmt(a.seen_pos)} s`);
    setDetailValue('Laatst gezien', `${fmt(a.seen)} s`);
    const live = document.getElementById('selectedLiveState');
    if (live) live.innerHTML = '<strong>● LIVE</strong><span>zojuist bijgewerkt</span>';
  }

  function installSelectionControls(a) {
    const detail = document.getElementById('detail');
    if (!detail) return;
    const h2 = detail.querySelector('h2');
    if (h2 && !document.getElementById('selectedLiveState')) {
      h2.insertAdjacentHTML('afterend', `<div class="selected-live-row"><div id="selectedLiveState"><strong>● LIVE</strong><span>zojuist bijgewerkt</span></div><button id="autoFollowButton" type="button">Auto Follow: ${autoFollow?'aan':'uit'}</button></div>`);
      document.getElementById('autoFollowButton').onclick = () => {
        autoFollow = !autoFollow;
        localStorage.setItem('adsbAutoFollow', autoFollow ? '1' : '0');
        document.getElementById('autoFollowButton').textContent = `Auto Follow: ${autoFollow?'aan':'uit'}`;
        if (autoFollow && Number.isFinite(Number(a.lat)) && Number.isFinite(Number(a.lon))) map.panTo([a.lat,a.lon]);
      };
    }
  }

  const originalShowDetailV06 = showDetail;
  showDetail = function showDetailV06(a) {
    stopPlayback();
    originalShowDetailV06(a);
    if (selectionHalo) map.removeLayer(selectionHalo);
    selectionHalo = L.circleMarker([a.lat, a.lon], {radius:18, weight:3, color:'#37ed9b', fill:false, opacity:.95, className:'selected-aircraft-halo'}).addTo(map);
    installSelectionControls(a);
    updateLiveDetail(a);
    const summary = document.getElementById('trackSummary');
    if (summary) summary.insertAdjacentHTML('afterend', '<div class="v06-playback"><button id="playTrack">▶ Track afspelen</button><button id="stopTrack">■ Stop</button></div>');
    document.getElementById('playTrack')?.addEventListener('click', () => {
      if (playbackTimer) stopPlayback();
      else {
        const active = document.querySelector('.track-buttons button.active');
        playTrack(a.hex, Number(active?.dataset.minutes || 30));
      }
    });
    document.getElementById('stopTrack')?.addEventListener('click', stopPlayback);
  };

  const originalUpdateMapV06 = updateMap;
  updateMap = function updateMapV06(items) {
    originalUpdateMapV06(items);
    if (!selectedHex) return;
    const selected = items.find(a => a.hex === selectedHex);
    if (!selected || !Number.isFinite(Number(selected.lat)) || !Number.isFinite(Number(selected.lon))) {
      const live = document.getElementById('selectedLiveState');
      if (live) live.innerHTML = '<strong class="stale">● GEEN LIVE POSITIE</strong><span>toestel tijdelijk niet ontvangen</span>';
      return;
    }
    if (selectionHalo) selectionHalo.setLatLng([selected.lat, selected.lon]);
    updateLiveDetail(selected);
    if (autoFollow) map.panTo([selected.lat, selected.lon], {animate:true, duration:.4});
  };

  setInterval(() => {
    const live = document.getElementById('selectedLiveState');
    if (!live || !selectedUpdatedAt) return;
    const age = Math.max(0, Math.round((Date.now()-selectedUpdatedAt)/1000));
    const span = live.querySelector('span');
    if (span) span.textContent = age < 2 ? 'zojuist bijgewerkt' : `${age} sec geleden bijgewerkt`;
  }, 1000);

  updateNightLayer();
  setInterval(updateNightLayer, 5 * 60 * 1000);
})();
