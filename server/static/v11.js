/* ADS-B Suite Aviation Layers */
(()=>{
  const layerNames={airports:'Luchthavens',navaids:'VOR / NDB',waypoints:'Waypoints',ctr:'CTR',tma:'TMA',fir:'FIR',restricted:'Restricted',danger:'Danger',prohibited:'Prohibited'};
  const defaults={airports:true,navaids:false,waypoints:false,ctr:false,tma:false,fir:false,restricted:false,danger:false,prohibited:false};
  const prefs=JSON.parse(localStorage.getItem('adsb-v11-layers')||'null')||defaults;
  const groups={};
  const radar=document.getElementById('radar');
  const toggle=document.createElement('button');toggle.className='v11-layer-button';toggle.type='button';toggle.textContent='☰ Kaartlagen';
  const panel=document.createElement('aside');panel.className='v11-panel';panel.hidden=true;
  panel.innerHTML=`<h3>Kaartlagen</h3><div class="v11-group">${Object.entries(layerNames).map(([k,v])=>`<label class="v11-toggle"><input type="checkbox" data-layer="${k}" ${prefs[k]?'checked':''}><span>${v}</span></label>`).join('')}</div><div class="v11-note">CTR/TMA/FIR en bijzondere gebieden worden geladen uit GeoJSON-bestanden in <code>/var/lib/adsb-suite/aviation</code>.</div><div id="v11LayerStatus" class="v11-layer-status">Lagen initialiseren…</div>`;
  radar.append(toggle,panel);
  toggle.onclick=()=>panel.hidden=!panel.hidden;

  function save(){localStorage.setItem('adsb-v11-layers',JSON.stringify(prefs))}
  function iconFor(p){
    if(p.type==='airport'||p.type==='military')return L.divIcon({className:'',html:`<span class="v11-airport-icon ${p.type==='military'?'military':''}">✈</span>`,iconSize:[22,22],iconAnchor:[11,11]});
    return L.divIcon({className:'',html:'<span class="v11-navaid-icon"><span>◆</span></span>',iconSize:[18,18],iconAnchor:[9,9]});
  }
  async function weather(icao,container){
    container.textContent='METAR/TAF laden…';
    try{const r=await fetch(`/api/aviation/weather/${encodeURIComponent(icao)}`);if(!r.ok)throw new Error(`HTTP ${r.status}`);const d=await r.json();
      const raw=x=>Array.isArray(x)&&x.length?(x[0].rawOb||x[0].rawTAF||x[0].raw_text||JSON.stringify(x[0])):'Geen gegevens';
      container.innerHTML=`<strong>METAR</strong><pre>${esc(raw(d.metar))}</pre><strong>TAF</strong><pre>${esc(raw(d.taf))}</pre>`;
    }catch(e){container.textContent=`Weer laden mislukt: ${e.message}`}
  }
  function popup(feature){const p=feature.properties||{},icao=p.icao||'';const div=document.createElement('div');div.className='v11-popup';div.innerHTML=`<strong>${esc(p.name||p.ident||p.icao||'Aviation object')}</strong><br>${icao?`ICAO: ${esc(icao)}<br>`:''}${p.iata?`IATA: ${esc(p.iata)}<br>`:''}${p.type?`Type: ${esc(p.type)}<br>`:''}`;if(icao){const box=document.createElement('div');box.className='v11-weather';const b=document.createElement('button');b.textContent='METAR / TAF laden';b.onclick=()=>weather(icao,box);box.appendChild(b);div.appendChild(box)}return div}
  const styles={ctr:{color:'#00d084',weight:2,fillOpacity:.05},tma:{color:'#3b82f6',weight:2,fillOpacity:.04},fir:{color:'#a78bfa',weight:2,dashArray:'8 5',fillOpacity:.02},restricted:{color:'#ff5f78',weight:2,fillOpacity:.10},danger:{color:'#ff9f43',weight:2,fillOpacity:.10},prohibited:{color:'#ff2d55',weight:3,fillOpacity:.12}};
  async function loadLayer(name){
    if(groups[name])return groups[name];
    const r=await fetch(`/api/aviation/layer/${name}`);if(!r.ok)throw new Error(`${name}: HTTP ${r.status}`);const data=await r.json();
    const group=L.geoJSON(data,{style:()=>styles[name]||{},pointToLayer:(f,ll)=>L.marker(ll,{icon:iconFor(f.properties||{})}),onEachFeature:(f,l)=>l.bindPopup(()=>popup(f))});
    groups[name]=group;return group;
  }
  async function setLayer(name,on){prefs[name]=on;save();const status=document.getElementById('v11LayerStatus');try{const group=await loadLayer(name);if(on)group.addTo(map);else map.removeLayer(group);status.textContent=`${layerNames[name]} ${on?'ingeschakeld':'uitgeschakeld'}.`}catch(e){status.textContent=`Laagfout: ${e.message}`}}
  panel.querySelectorAll('[data-layer]').forEach(cb=>cb.onchange=()=>setLayer(cb.dataset.layer,cb.checked));
  Promise.all(Object.keys(layerNames).filter(k=>prefs[k]).map(k=>setLayer(k,true))).then(()=>document.getElementById('v11LayerStatus').textContent='Kaartlagen gereed.');
})();
