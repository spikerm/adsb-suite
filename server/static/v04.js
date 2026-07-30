/* ADS-B Suite v0.4: route and aircraft photo enrichment. */
(() => {
  const originalShowDetail = showDetail;
  const airportName = airport => {
    if (!airport) return '–';
    return [airport.iata || airport.icao, airport.municipality || airport.name, airport.country]
      .filter(Boolean).join(' · ');
  };

  async function loadV04Enrichment(aircraft) {
    let box = document.getElementById('enrichment');
    if (!box) {
      box = document.createElement('div');
      box.id = 'enrichment';
      const detail = document.getElementById('detail');
      const subtitle = detail?.querySelector('.aircraft-subtitle');
      if (subtitle) subtitle.insertAdjacentElement('afterend', box);
    }
    if (!box) return;
    box.innerHTML = '<p class="muted">Route en foto laden…</p>';
    try {
      const response = await fetch(`/api/enrichment/${encodeURIComponent(aircraft.hex)}?flight=${encodeURIComponent(aircraft.flight || '')}`);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      const route = data.route;
      const photo = data.photo;
      let html = '';
      if (photo?.image) {
        html += `<figure class="aircraft-photo"><a href="${esc(photo.link || photo.image)}" target="_blank" rel="noopener"><img src="${esc(photo.image)}" alt="${esc(aircraft.registration || aircraft.flight || aircraft.hex)}"></a><figcaption>Foto © ${esc(photo.photographer || 'Planespotters.net')}</figcaption></figure>`;
      }
      if (route) {
        html += `<section class="route-card"><h3>Route</h3><div class="route-line"><strong>${esc(route.origin?.iata || route.origin?.icao || '–')}</strong><span>→</span><strong>${esc(route.destination?.iata || route.destination?.icao || '–')}</strong></div><dl><dt>Vertrek</dt><dd>${esc(airportName(route.origin))}</dd><dt>Bestemming</dt><dd>${esc(airportName(route.destination))}</dd><dt>Maatschappij</dt><dd>${esc(route.airline?.name || aircraft.operator || '–')}</dd></dl></section>`;
      }
      box.innerHTML = html || '<p class="muted">Geen route of foto gevonden.</p>';
    } catch (error) {
      console.error('V0.4 enrichment error', error);
      box.innerHTML = '<p class="muted">Route en foto tijdelijk niet beschikbaar.</p>';
    }
  }

  showDetail = function showDetailV04(aircraft) {
    originalShowDetail(aircraft);
    loadV04Enrichment(aircraft);
  };
})();
