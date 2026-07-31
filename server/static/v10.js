/* ADS-B Suite Flight Replay */
(()=>{
  const originalApplyV10=apply;
  let replayActive=false,liveSnapshot=null,replay=null,frameTimes=[],frameIndex=0,timer=null,speed=1,periodHours=1;
  const trails=new Map();
  const trailLayers=new Map();

  apply=function(data){liveSnapshot=data;if(!replayActive)originalApplyV10(data)};

  const nav=document.querySelector('nav');
  const button=document.createElement('button');
  button.dataset.view='replay';button.textContent='Flight Replay';
  nav.insertBefore(button,document.getElementById('toggleLabels'));
  const radar=document.getElementById('radar');
  radar.insertAdjacentHTML('beforebegin',`<section id="replayControls" class="v10-replay-shell" hidden><div class="v10-replay"><article class="v10-replay-panel"><div class="v10-replay-head"><h2>Flight Replay</h2><span id="v10Mode" class="v10-mode live">● LIVE</span></div><div class="v10-periods"><button data-hours="1" class="active">1 uur</button><button data-hours="6">6 uur</button><button data-hours="24">24 uur</button></div><div class="v10-controls"><button id="v10Load">Replay laden</button><button id="v10Play">▶ Afspelen</button><select id="v10Speed"><option value="1">1×</option><option value="2">2×</option><option value="4">4×</option><option value="8">8×</option><option value="16">16×</option></select><input id="v10Timeline" type="range" min="0" max="0" value="0"><span id="v10Time" class="v10-time">–</span></div><div id="v10Status" class="v10-status">Kies een periode en laad de replay.</div></article><article class="v10-replay-panel"><div class="v10-map-note"><span>De replay gebruikt dezelfde radar-kaart. Klik tijdens replay op een vliegtuig voor details.</span><button id="v10Live">Terug naar LIVE</button></div></article></div></section>`);
  const controls=document.getElementById('replayControls');

  function openReplay(){
    document.querySelectorAll('[data-view]').forEach(x=>x.classList.toggle('active',x===button));
    document.querySelectorAll('.view').forEach(x=>x.classList.toggle('active',x.id==='radar'));
    controls.hidden=false;setTimeout(()=>map.invalidateSize(),80);
  }
  button.onclick=openReplay;
  nav.addEventListener('click',e=>{const target=e.target.closest('[data-view]');if(target&&target!==button)controls.hidden=true},true);

  function clearTrails(){trailLayers.forEach(layer=>map.removeLayer(layer));trailLayers.clear();trails.clear()}
  function stop(){if(timer)clearTimeout(timer);timer=null;document.getElementById('v10Play').textContent='▶ Afspelen'}
  function renderFrame(index){
    if(!replay||!frameTimes.length)return;
    frameIndex=Math.max(0,Math.min(frameTimes.length-1,index));
    const ts=frameTimes[frameIndex],items=replay.frames[String(ts)]||[];
    aircraft=items;updateMap(items);updateTable();
    items.forEach(a=>{
      if(!Number.isFinite(Number(a.lat))||!Number.isFinite(Number(a.lon)))return;
      const pts=trails.get(a.hex)||[];pts.push([Number(a.lat),Number(a.lon)]);if(pts.length>30)pts.shift();trails.set(a.hex,pts);
      let layer=trailLayers.get(a.hex);if(!layer){layer=L.polyline(pts,{weight:2,opacity:.55,className:'v10-track'}).addTo(map);trailLayers.set(a.hex,layer)}else layer.setLatLngs(pts);
    });
    document.getElementById('v10Timeline').value=frameIndex;
    document.getElementById('v10Time').textContent=new Date(ts*1000).toLocaleString('nl-NL');
    document.getElementById('v10Status').textContent=`Frame ${frameIndex+1}/${frameTimes.length} · ${items.length} vliegtuigen · ${replay.observation_count} observaties geladen`;
  }
  function schedule(){
    stop();document.getElementById('v10Play').textContent='⏸ Pauzeren';
    const tick=()=>{if(frameIndex>=frameTimes.length-1){stop();return}renderFrame(frameIndex+1);timer=setTimeout(tick,Math.max(80,800/speed))};
    timer=setTimeout(tick,Math.max(80,800/speed));
  }
  async function loadReplay(){
    stop();clearTrails();openReplay();
    const status=document.getElementById('v10Status');status.className='v10-status';status.textContent='Historische gegevens laden…';
    try{
      const end=Math.floor(Date.now()/1000),start=end-periodHours*3600,step=periodHours>=24?30:periodHours>=6?20:10;
      const r=await fetch(`/api/replay?start=${start}&end=${end}&step=${step}`);if(!r.ok)throw new Error((await r.text())||`HTTP ${r.status}`);
      replay=await r.json();frameTimes=Object.keys(replay.frames||{}).map(Number).sort((a,b)=>a-b);
      if(!frameTimes.length)throw new Error('Geen historische posities in deze periode gevonden.');
      replayActive=true;document.body.classList.add('v10-replay-active');
      document.getElementById('v10Mode').className='v10-mode';document.getElementById('v10Mode').textContent='● REPLAY';
      const slider=document.getElementById('v10Timeline');slider.max=String(frameTimes.length-1);slider.value='0';
      renderFrame(0);map.setView([receiver.lat,receiver.lon],9);
    }catch(e){status.className='v10-status bad';status.textContent=`Replay laden mislukt: ${e.message}`}
  }
  function returnLive(){
    stop();clearTrails();replayActive=false;replay=null;frameTimes=[];document.body.classList.remove('v10-replay-active');
    document.getElementById('v10Mode').className='v10-mode live';document.getElementById('v10Mode').textContent='● LIVE';
    document.getElementById('v10Status').textContent='Live modus actief.';
    if(liveSnapshot)originalApplyV10(liveSnapshot);
    controls.hidden=true;document.querySelector('[data-view="radar"]').click();
  }

  document.querySelectorAll('.v10-periods button').forEach(b=>b.onclick=()=>{document.querySelectorAll('.v10-periods button').forEach(x=>x.classList.remove('active'));b.classList.add('active');periodHours=Number(b.dataset.hours)});
  document.getElementById('v10Load').onclick=loadReplay;
  document.getElementById('v10Play').onclick=()=>{if(!replayActive||!frameTimes.length)return loadReplay();if(timer)stop();else schedule()};
  document.getElementById('v10Speed').onchange=e=>{speed=Number(e.target.value)||1;if(timer)schedule()};
  document.getElementById('v10Timeline').oninput=e=>{stop();clearTrails();renderFrame(Number(e.target.value))};
  document.getElementById('v10Live').onclick=returnLive;
})();