// Minimal client logic: search, one-hop, draw map
const resultBody = document.getElementById('resultBody');
const statusEl = document.getElementById('status');
const map = L.map('map').setView([20,0], 2);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png').addTo(map);
let markers = [], polylines = [];

function clearMap() {
  markers.forEach(m => map.removeLayer(m)); markers = [];
  polylines.forEach(p => map.removeLayer(p)); polylines = [];
}

async function search() {
  const stype = document.getElementById('stype').value;
  const code = document.getElementById('code').value.trim().toUpperCase();
  if (!code) { statusEl.textContent = 'Enter IATA or name'; return; }
  statusEl.textContent = 'Loading...';
  try {
    const res = await fetch(`/${stype}/${encodeURIComponent(code)}`);
    if (!res.ok) {
      resultBody.innerHTML = `<div class="text-danger">Not found</div>`;
      statusEl.textContent = '';
      return;
    }
    const data = await res.json();
    renderResult(stype, data);
    statusEl.textContent = 'Done';
  } catch (e) {
    statusEl.textContent = 'Network error: ' + e.message;
  }
}

async function onehop() {
  const src = document.getElementById('one-src').value.trim().toUpperCase();
  const dst = document.getElementById('one-dst').value.trim().toUpperCase();
  if (!src || !dst) return statusEl.textContent = 'Enter both IATA codes';
  statusEl.textContent = 'Searching...';
  try {
    const res = await fetch(`/onehop/${src}/${dst}`);
    if (!res.ok) { resultBody.innerHTML = `<div class="text-danger">No routes</div>`; statusEl.textContent=''; return; }
    const data = await res.json();
    renderOnehop(data);
    statusEl.textContent = 'Done';
  } catch (e) {
    statusEl.textContent = 'Network error: ' + e.message;
  }
}

function renderResult(type, data) {
  clearMap();
  if (type === 'airline') {
    resultBody.innerHTML = `<h6>${data.name} (${data.iata})</h6><pre>${JSON.stringify(data,null,2)}</pre>`;
  } else {
    resultBody.innerHTML = `<h6>${data.name} (${data.iata})</h6><pre>${JSON.stringify(data,null,2)}</pre>`;
    if (data.latitude && data.longitude) {
      const m = L.marker([data.latitude, data.longitude]).addTo(map);
      markers.push(m);
      map.setView([data.latitude, data.longitude], 8);
    }
  }
}

function renderOnehop(data) {
  clearMap();
  resultBody.innerHTML = `<h6>${data.source} → ${data.destination}</h6>`;
  if (!data.routes || data.routes.length===0) { resultBody.innerHTML += '<div>No one-hop routes found</div>'; return; }
  data.routes.slice(0,50).forEach(r => {
    const div = document.createElement('div');
    div.className = 'mb-2';
    div.innerHTML = `<strong>${r.intermediate_iata}</strong> — ${r.airline1} / ${r.airline2} — ${r.total_distance_miles.toFixed(1)} mi`;
    resultBody.appendChild(div);
    // try plotting if coordinates available via /airport endpoint
    (async ()=>{
      try {
        const a1 = await (await fetch(`/airport/${r.intermediate_iata}`)).json();
        const src = data.source.match(/\(([^)]+)\)/); // "Name (IATA)"
        const dst = data.destination.match(/\(([^)]+)\)/);
        if (a1.latitude && a1.longitude) {
          const m = L.marker([a1.latitude, a1.longitude]).addTo(map);
          markers.push(m);
        }
        if (src) {
          const s = await (await fetch(`/airport/${src[1]}`)).json();
          if (s.latitude && s.longitude) {
            const p = L.polyline([[s.latitude, s.longitude],[a1.latitude, a1.longitude]], {color:'blue'}).addTo(map);
            polylines.push(p);
          }
        }
        if (dst) {
          const d = await (await fetch(`/airport/${dst[1]}`)).json();
          if (d.latitude && d.longitude) {
            const p2 = L.polyline([[a1.latitude, a1.longitude],[d.latitude, d.longitude]], {color:'green'}).addTo(map);
            polylines.push(p2);
          }
        }
      } catch (e) { /* ignore plotting errors */ }
    })();
  });
}

// wire UI
document.getElementById('searchBtn').addEventListener('click', search);
document.getElementById('oneBtn').addEventListener('click', onehop);

// Allow Enter key to trigger search
document.getElementById('code').addEventListener('keypress', function(e) {
  if (e.key === 'Enter') search();
});
document.getElementById('one-src').addEventListener('keypress', function(e) {
  if (e.key === 'Enter') onehop();
});
document.getElementById('one-dst').addEventListener('keypress', function(e) {
  if (e.key === 'Enter') onehop();
});

// --- Autocomplete for airlines ---
(function setupAirlineAutocomplete(){
  const stypeEl = document.getElementById('stype');
  const input = document.getElementById('code');
  const list = document.getElementById('airline-suggest');
  if (!stypeEl || !input || !list) return;

  let controller = null;
  async function suggest(q){
    try {
      if (controller) controller.abort();
      controller = new AbortController();
      const resp = await fetch(`/api/airlines/suggest?q=${encodeURIComponent(q)}`, {signal: controller.signal});
      if (!resp.ok) return;
      const data = await resp.json();
      list.innerHTML = '';
      (data.items || []).forEach(it => {
        const opt = document.createElement('option');
        const tag = (it.iata && it.iata.length) ? it.iata : (it.icao || '');
        opt.value = tag;
        opt.label = `${it.name}${tag ? ' ('+tag+')' : ''}`;
        list.appendChild(opt);
      });
    } catch(e){ /* ignore aborted */ }
  }

  input.addEventListener('input', (e) => {
    const mode = stypeEl.value;
    const q = e.target.value.trim();
    if (mode !== 'airline') return;
    if (q.length < 1) { list.innerHTML=''; return; }
    suggest(q);
  });

  // If the user switches to airline mode, trigger suggest
  stypeEl.addEventListener('change', () => {
    if (stypeEl.value === 'airline' && input.value.trim().length >= 1) {
      suggest(input.value.trim());
    } else {
      list.innerHTML='';
    }
  });
})();


// --- Enhanced autocomplete: always suggest airlines + fallback panel if <datalist> isn't shown ---
(function enhancedAutocomplete(){
  const input = document.getElementById('code');
  const nativeList = document.getElementById('airline-suggest');
  if (!input) return;

  // Fallback dropdown UI
  let panel = document.getElementById('airline-suggest-panel');
  if (!panel) {
    panel = document.createElement('div');
    panel.id = 'airline-suggest-panel';
    panel.style.position = 'absolute';
    panel.style.zIndex = '9999';
    panel.style.display = 'none';
    panel.style.maxHeight = '200px';
    panel.style.overflowY = 'auto';
    panel.className = 'list-group shadow';
    document.body.appendChild(panel);
    // Position near input
    function positionPanel(){
      const r = input.getBoundingClientRect();
      panel.style.left = (window.scrollX + r.left) + 'px';
      panel.style.top = (window.scrollY + r.bottom + 2) + 'px';
      panel.style.width = r.width + 'px';
    }
    positionPanel();
    window.addEventListener('resize', positionPanel);
    window.addEventListener('scroll', positionPanel, true);
    input.addEventListener('focus', positionPanel);
  }

  let controller = null;
  async function suggest(q){
    try {
      if (controller) controller.abort();
      controller = new AbortController();
      const resp = await fetch(`/api/airlines/suggest?q=${encodeURIComponent(q)}`, {signal: controller.signal});
      if (!resp.ok) return;
      const data = await resp.json();
      // Update native datalist (if supported)
      if (nativeList) {
        nativeList.innerHTML = '';
        (data.items || []).forEach(it => {
          const opt = document.createElement('option');
          const code = it.iata && it.iata.length ? it.iata : (it.icao || '');
          opt.value = code;
          opt.label = `${it.name}${code ? ' ('+code+')' : ''}`;
          nativeList.appendChild(opt);
        });
      }
      // Update fallback panel
      panel.innerHTML = '';
      (data.items || []).forEach(it => {
        const code = it.iata && it.iata.length ? it.iata : (it.icao || '');
        const a = document.createElement('button');
        a.type = 'button';
        a.className = 'list-group-item list-group-item-action';
        a.textContent = `${it.name}${code ? ' ('+code+')' : ''}`;
        a.addEventListener('click', () => { input.value = code; panel.style.display='none'; input.focus(); });
        panel.appendChild(a);
      });
      panel.style.display = (panel.children.length > 0) ? 'block' : 'none';
    } catch(e){ /* ignore */ }
  }

  input.addEventListener('input', (e) => {
    const q = e.target.value.trim();
    if (q.length < 1) { if (nativeList) nativeList.innerHTML=''; panel.style.display='none'; return; }
    suggest(q);
  });

  // Hide panel when clicking elsewhere
  document.addEventListener('click', (e) => {
    if (e.target === input || panel.contains(e.target)) return;
    panel.style.display = 'none';
  });
})();

// --- Airport autocomplete for one-hop inputs (IATA/ICAO/name/city) ---
(function setupAirportAutocomplete(){
  const list = document.getElementById('airport-suggest');
  const src = document.getElementById('one-src');
  const dst = document.getElementById('one-dst');
  if (!list || !src || !dst) return;

  let controller = null;
  async function suggest(q){
    try{
      if (controller) controller.abort();
      controller = new AbortController();
      const resp = await fetch(`/api/airports/suggest?q=${encodeURIComponent(q)}`, {signal: controller.signal});
      if (!resp.ok) return;
      const data = await resp.json();
      list.innerHTML = '';
      (data.items||[]).forEach(it => {
        const opt = document.createElement('option');
        const code = it.iata && it.iata.length ? it.iata : (it.icao || '');
        const city = it.city ? (' - '+it.city) : '';
        opt.value = code;
        opt.label = `${it.name}${city}${code ? ' ('+code+')' : ''}`;
        list.appendChild(opt);
      });
    }catch(e){}
  }
  function wire(el){
    el.addEventListener('input', e => {
      const q = e.target.value.trim();
      if (q.length < 1) { list.innerHTML=''; return; }
      suggest(q);
    });
  }
  wire(src); wire(dst);
})();
