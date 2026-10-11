// Glue shared by the ysp web demos: the Emscripten Module object, the
// status line every page shows, start, stop and downloads. Each page sets
// window.yspPage = { name, args() } before this file and loads its demo's
// .js after it.
(function () {
  'use strict';
  const $ = (id) => document.getElementById(id);

  // The smallest step performance.now() shows in 20 ms: the resolution of
  // every time the demo records. Browsers coarsen it when the page is not
  // cross-origin isolated, so it is measured here, never assumed.
  function clockStepUs() {
    let prev = performance.now(), min = Infinity;
    const end = prev + 20;
    for (;;) {
      const t = performance.now();
      if (t > end) break;
      if (t !== prev) { if (t - prev < min) min = t - prev; prev = t; }
    }
    return Math.round(min * 1000 * 10) / 10;
  }

  const status = {
    coi: window.crossOriginIsolated === true,
    clock_step_us: clockStepUs(),
    build: 'asyncify, no threads',
    audio: 'none',
    service_worker: !!(navigator.serviceWorker && navigator.serviceWorker.controller),
    user_agent: navigator.userAgent,
  };

  const logLines = [];
  function log(text, isErr) {
    logLines.push((isErr ? '! ' : '') + text);
    const el = $('log');
    if (el) { el.textContent = logLines.join('\n'); el.scrollTop = el.scrollHeight; }
    if (isErr) console.error(text); else console.log(text);
  }

  function showStatus() {
    const el = $('status');
    if (!el) return;
    el.innerHTML = '';
    const items = [
      ['cross-origin isolated', status.coi ? 'yes' : 'no'],
      ['clock step', status.clock_step_us + ' µs'],
      ['audio', status.audio],
      ['build', status.build],
    ];
    for (const [k, v] of items) {
      const s = document.createElement('span');
      s.textContent = k + ': ' + v;
      el.appendChild(s);
    }
  }

  function download(name, text, type) {
    const blob = new Blob([text], { type: type || 'text/plain' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = name;
    document.body.appendChild(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
  }

  // The session log: what the page and the screen said about this browser.
  function sessionText() {
    const lines = Object.entries(status).map(([k, v]) => k + '=' + v);
    if (window.Module && Module.yspScreen) lines.push('screen=' + Module.yspScreen);
    lines.push('', ...logLines);
    return lines.join('\n') + '\n';
  }

  // Simple CSV for the results table: the demos write RFC 4180 with quotes
  // only around fields that need them.
  function parseCsv(text) {
    const rows = [];
    let row = [], f = '', q = false;
    for (let i = 0; i < text.length; i++) {
      const c = text[i];
      if (q) {
        if (c === '"' && text[i + 1] === '"') { f += '"'; i++; }
        else if (c === '"') q = false;
        else f += c;
      } else if (c === '"') q = true;
      else if (c === ',') { row.push(f); f = ''; }
      else if (c === '\n') { row.push(f); rows.push(row); row = []; f = ''; }
      else f += c;
    }
    if (f || row.length) { row.push(f); rows.push(row); }
    return rows;
  }

  function table(el, rows, cols) {
    const head = rows[0], idx = cols.map((c) => head.indexOf(c));
    const t = document.createElement('table');
    const tr = t.insertRow();
    cols.forEach((c) => { const th = document.createElement('th'); th.textContent = c; tr.appendChild(th); });
    rows.slice(1).forEach((r) => {
      const row = t.insertRow();
      idx.forEach((i) => { row.insertCell().textContent = i >= 0 ? r[i] : ''; });
    });
    el.innerHTML = '';
    el.appendChild(t);
  }

  function start(opts) {
    const page = window.yspPage || {};
    const args = page.args ? page.args() : [];
    // Fullscreen needs this click: a page may not ask without a gesture.
    if (opts && opts.fullscreen && $('stage').requestFullscreen) $('stage').requestFullscreen().catch(() => {});
    document.body.classList.add('running');
    if ($('intro')) $('intro').classList.add('hidden');
    $('stage').classList.add('running');
    if ($('stop')) $('stop').disabled = false;
    log('start ' + (page.name || '') + ' ' + args.join(' '));
    setTimeout(() => Module.callMain(args), 0);
  }

  function stop() {
    if (Module._yscr_request_abort) Module._yscr_request_abort();
  }

  function finish() {
    $('stage').classList.remove('running');
    if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
    if ($('stop')) $('stop').disabled = true;
    if ($('results')) $('results').classList.remove('hidden');
  }

  window.ysp = { status, log, download, sessionText, parseCsv, table, start, stop, finish, showStatus };

  window.Module = Object.assign({
    canvas: $('canvas'),
    noInitialRun: true,
    print: (t) => log(t),
    printErr: (t) => log(t, true),
    onRuntimeInitialized() {
      const b = $('start');
      if (b) { b.disabled = false; b.textContent = b.dataset.ready || 'Start'; }
    },
    onAbort(what) { log('aborted: ' + what, true); },
  }, window.Module || {});

  document.addEventListener('DOMContentLoaded', showStatus);
  if (document.readyState !== 'loading') showStatus();
})();
