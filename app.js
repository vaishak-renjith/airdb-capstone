/* Minimal front-end glue for the Air Travel DB UI.
   - One-hop: calls /onehop/<SRC>/<DST>
   - Airline lookup: /airline/<TERM>
   - Airport lookup: /airport/<TERM>
   - Autocomplete using /api/airlines/suggest and /api/airports/suggest
*/

(function () {
  const byId = (id) => document.getElementById(id);

  const stypeEl = byId('stype');
  const codeEl = byId('code');
  const searchBtn = byId('searchBtn');
  const statusEl = byId('status');

  const oneSrcEl = byId('one-src');
  const oneDstEl = byId('one-dst');
  const oneBtn = byId('oneBtn');

  const resultBody = byId('resultBody');

  // -------- Utilities --------
  function setStatus(msg) { statusEl.textContent = msg || ''; }
  function clearResults() { resultBody.innerHTML = ''; }

  function renderJSON(data) {
    const pre = document.createElement('pre');
    pre.className = 'mb-0';
    pre.textContent = JSON.stringify(data, null, 2);
    resultBody.appendChild(pre);
  }

  function renderOneHop(src, dst, rows) {
    clearResults();
    const h = document.createElement('div');
    h.className = 'mb-2 fw-semibold';
    h.textContent = `${src} → ${dst}`;
    resultBody.appendChild(h);

    if (!rows || rows.length === 0) {
      const p = document.createElement('div');
      p.textContent = 'No one-hop routes found';
      resultBody.appendChild(p);
      return;
    }

    const table = document.createElement('table');
    table.className = 'table table-sm table-striped';
    const thead = document.createElement('thead');
    thead.innerHTML = `<tr>
        <th>#</th><th>Via</th><th>Leg1 Airline</th><th>Leg2 Airline</th><th>Total Miles</th>
      </tr>`;
    table.appendChild(thead);

    const tbody = document.createElement('tbody');
    rows.forEach((r, i) => {
      const tr = document.createElement('tr');
      tr.innerHTML = `<td>${i + 1}</td>
        <td>${r.via}</td>
        <td>${r.leg1_airline}</td>
        <td>${r.leg2_airline}</td>
        <td>${r.total_miles}</td>`;
      tbody.appendChild(tr);
    });
    table.appendChild(tbody);
    resultBody.appendChild(table);
  }

  async function getJSON(url) {
    const res = await fetch(url);
    if (!res.ok) {
      throw new Error(`${res.status} ${res.statusText}`);
    }
    return res.json();
  }

  // -------- Suggestions --------
  // Populate airline suggestions while typing in the top "code" box when Airline is selected
  codeEl.addEventListener('input', async () => {
    try {
      const type = stypeEl.value;
      if (type !== 'airline') return;

      const q = codeEl.value.trim();
      const box = document.getElementById('airline-suggest');
      if (!q) { box.innerHTML = ''; return; }

      const data = await getJSON(`/api/airlines/suggest?q=${encodeURIComponent(q)}`);
      box.innerHTML = '';
      (data.items || []).forEach(it => {
        const opt = document.createElement('option');
        opt.value = it.iata || it.icao || it.name;
        opt.label = `${it.name}${it.iata ? ` (${it.iata})` : ''}${it.icao ? ` / ${it.icao}` : ''}`;
        box.appendChild(opt);
      });
    } catch (_) { /* ignore */ }
  });

  // Populate airport suggestions for both one-hop inputs
  function attachAirportSuggest(inputEl) {
    inputEl.addEventListener('input', async () => {
      try {
        const q = inputEl.value.trim();
        const box = document.getElementById('airport-suggest');
        if (!q) { box.innerHTML = ''; return; }
        const data = await getJSON(`/api/airports/suggest?q=${encodeURIComponent(q)}`);
        box.innerHTML = '';
        (data.items || []).forEach(it => {
          const opt = document.createElement('option');
          opt.value = it.iata || it.icao || it.name;
          opt.label = `${it.name}${it.city ? ` – ${it.city}` : ''}${it.iata ? ` (${it.iata})` : ''}${it.icao ? ` / ${it.icao}` : ''}`;
          box.appendChild(opt);
        });
      } catch (_) { /* ignore */ }
    });
  }
  attachAirportSuggest(oneSrcEl);
  attachAirportSuggest(oneDstEl);

  // -------- Search buttons --------
  searchBtn.addEventListener('click', async () => {
    clearResults();
    const type = stypeEl.value;
    const term = codeEl.value.trim();
    if (!term) { setStatus('Enter an airline/airport'); return; }

    try {
      setStatus('Searching…');
      if (type === 'airline') {
        const data = await getJSON(`/airline/${encodeURIComponent(term)}`);
        renderJSON(data);
      } else {
        const data = await getJSON(`/airport/${encodeURIComponent(term)}`);
        renderJSON(data);
      }
      setStatus('Done');
    } catch (err) {
      setStatus('Not found');
      resultBody.textContent = `Error: ${err.message}`;
    }
  });

  oneBtn.addEventListener('click', async () => {
    clearResults();
    const src = oneSrcEl.value.trim().toUpperCase();
    const dst = oneDstEl.value.trim().toUpperCase();
    if (!src || !dst) {
      resultBody.textContent = 'Please provide both Source and Destination IATA codes.';
      return;
    }

    if (src === dst) {
        resultBody.textContent = 'Source and destination must be different for a one-hop search.';
        return;
    }

    try {
      setStatus('Searching one-hop…');
      const data = await getJSON(`/onehop/${encodeURIComponent(src)}/${encodeURIComponent(dst)}`);
      renderOneHop(src, dst, data);
      setStatus('Done');
    } catch (err) {
      setStatus('Error');
      resultBody.textContent = `Error: ${err.message}`;
    }
  });
})();
