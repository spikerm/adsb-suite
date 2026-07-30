/* ADS-B Suite v0.5: richer detail panel, charts and map layers. */
(() => {
  const baseLayers = {};
  map.eachLayer(layer => {
    if (layer instanceof L.TileLayer) baseLayers['OpenStreetMap'] = layer;
  });
  const topo = L.tileLayer('https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png', {
    maxZoom: 17,
    attribution: '© OpenStreetMap · OpenTopoMap'
  });
  const satellite = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
    maxZoom: 19,
    attribution: 'Tiles © Esri'
  });
  baseLayers['Topografisch'] = topo;
  baseLayers['Satelliet'] = satellite;
  L.control.layers(baseLayers, {}, {position: 'topleft', collapsed: true}).addTo(map);
  L.control.scale({imperial: false, position: 'bottomleft'}).addTo(map);

  const originalShowDetailV05 = showDetail;
  const originalLoadTrackV05 = loadTrack;
  let detailRequest = 0;

  const svgChart = (points, key, labelText, unit) => {
    const values = points.map(p => Number(p[key])).filter(Number.isFinite);
    if (values.length < 2) return `<div class="chart-empty">Geen ${esc(labelText.toLowerCase())} beschikbaar</div>`;
    const width = 520, height = 150, pad = 24;
    const min = Math.min(...values), max = Math.max(...values), span = Math.max(max - min, 1);
    const coords = points.map((p, i) => {
      const value = Number(p[key]);
      if (!Number.isFinite(value)) return null;
      const x = pad + (i / Math.max(points.length - 1, 1)) * (width - pad * 2);
      const y = height - pad - ((value - min) / span) * (height - pad * 2);
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    }).filter(Boolean).join(' ');
    return `<div class="mini-chart"><div class="chart-title"><strong>${esc(labelText)}</strong><span>${fmt(min,0)}–${fmt(max,0)} ${unit}</span></div><svg viewBox="0 0 ${width} ${height}" role="img" aria-label="${esc(labelText)}"><line x1="${pad}" y1="${height-pad}" x2="${width-pad}" y2="${height-pad}" class="chart-axis"/><polyline points="${coords}" class="chart-line"/></svg></div>`;
  };

  async function loadCharts(hex, minutes) {
    const target = document.getElementById('flightCharts');
    if (!target) return;
    target.innerHTML = '<p class="muted">Grafieken laden…</p>';
    try {
      const response = await fetch(`/api/track/${encodeURIComponent(hex)}?minutes=${minutes}`);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      const points = data.points || [];
      target.innerHTML = svgChart(points, 'altitude_ft', 'Hoogteprofiel', 'ft') + svgChart(points, 'speed_kt', 'Snelheidsprofiel', 'kt');
    } catch (error) {
      console.error('V0.5 charts error', error);
      target.innerHTML = '<p class="muted">Grafieken tijdelijk niet beschikbaar.</p>';
    }
  }

  loadTrack = async function loadTrackV05(hex, minutes = 30) {
    await originalLoadTrackV05(hex, minutes);
    await loadCharts(hex, minutes);
  };

  async function enrichDetail(aircraft, requestId) {
    const facts = document.getElementById('v05Facts');
    if (facts) facts.innerHTML = '<p class="muted">Extra vluchtgegevens laden…</p>';
    try {
      const response = await fetch(`/api/enrichment/${encodeURIComponent(aircraft.hex)}?flight=${encodeURIComponent(aircraft.flight || '')}`);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      if (requestId !== detailRequest) return;
      const route = data.route;
      const airline = route?.airline?.name || first(aircraft.operator, aircraft.ownOp);
      if (facts) facts.innerHTML = `<dl><dt>Vluchtnummer</dt><dd>${esc(route?.number || aircraft.flight || '–')}</dd><dt>Maatschappij</dt><dd>${esc(airline || '–')}</dd><dt>Bouwjaar</dt><dd>${esc(aircraft.year || '–')}</dd><dt>Registratieland</dt><dd>${esc(aircraft.country || route?.airline?.country || '–')}</dd><dt>Routebron bijgewerkt</dt><dd>${data.updated_at ? new Date(data.updated_at * 1000).toLocaleTimeString('nl-NL') : '–'}</dd></dl>`;
      const detail = document.getElementById('detail');
      const operatorRows = [...detail.querySelectorAll('dt')].filter(x => x.textContent === 'Operator');
      operatorRows.forEach(dt => {
        const dd = dt.nextElementSibling;
        if (dd && (!dd.textContent.trim() || dd.textContent.trim() === '–') && airline) dd.textContent = airline;
      });
    } catch (error) {
      console.error('V0.5 detail enrichment error', error);
      if (facts) facts.innerHTML = '<p class="muted">Extra vluchtgegevens niet beschikbaar.</p>';
    }
  }

  showDetail = function showDetailV05(aircraft) {
    detailRequest += 1;
    const requestId = detailRequest;
    originalShowDetailV05(aircraft);
    const detail = document.getElementById('detail');
    const subtitle = detail.querySelector('.aircraft-subtitle');
    if (subtitle) {
      subtitle.insertAdjacentHTML('afterend', `<section class="v05-live-card"><div><small>Hoogte</small><strong>${fmt(first(aircraft.altitude_ft, aircraft.alt_baro),0)} ft</strong></div><div><small>Snelheid</small><strong>${fmt(first(aircraft.speed_kt, aircraft.gs),0)} kt</strong></div><div><small>Afstand</small><strong>${fmt(aircraft.distance_km)} km</strong></div><div><small>Koers</small><strong>${fmt(first(aircraft.track_deg, aircraft.track),0)}°</strong></div></section><section id="v05Facts" class="v05-facts"></section>`);
    }
    const summary = document.getElementById('trackSummary');
    if (summary) summary.insertAdjacentHTML('afterend', '<section id="flightCharts" class="flight-charts"><p class="muted">Grafieken laden…</p></section>');
    detail.querySelectorAll('[data-minutes]').forEach(button => {
      button.addEventListener('click', () => loadCharts(aircraft.hex, Number(button.dataset.minutes)));
    });
    enrichDetail(aircraft, requestId);
    loadCharts(aircraft.hex, 30);
  };

  map.on('zoomend', () => {
    const size = Math.max(24, Math.min(38, 22 + map.getZoom()));
    document.documentElement.style.setProperty('--plane-size', `${size}px`);
  });
})();
