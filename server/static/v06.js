/* ADS-B Suite v0.6: playback, selected-aircraft halo and day/night overlay. */
(() => {
  let selectionHalo = null;
  let playbackMarker = null;
  let playbackTimer = null;
  let nightLayer = null;

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

  const originalShowDetailV06 = showDetail;
  showDetail = function showDetailV06(a) {
    stopPlayback();
    originalShowDetailV06(a);
    if (selectionHalo) map.removeLayer(selectionHalo);
    selectionHalo = L.circleMarker([a.lat, a.lon], {radius:18, weight:3, color:'#37ed9b', fill:false, opacity:.95, className:'selected-aircraft-halo'}).addTo(map);
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

  updateNightLayer();
  setInterval(updateNightLayer, 5 * 60 * 1000);
})();
