// Glue shared by the ysp web demos: the Emscripten Module object, the
// status line every page shows, the build a page loads, start, stop, the
// stage that holds the canvas during a run, and downloads. Each page sets
// window.yspPage = { name, args(), builds } before this file; builds is
// { coi: 'x_mt.js', fallback: 'x.js' } (coi only where a threaded build
// exists) and this file loads the one the page can run.
(function () {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const page = window.yspPage || {};

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

  // A threaded build needs SharedArrayBuffer, which a browser gives only to
  // a cross-origin isolated page; every other page gets the fallback.
  const coi = window.crossOriginIsolated === true;
  const builds = page.builds || {};
  const threaded = !!(coi && builds.coi && typeof SharedArrayBuffer !== 'undefined');
  const script = threaded ? builds.coi : builds.fallback;
  const coarse = window.matchMedia && matchMedia('(pointer: coarse)').matches;

  const status = {
    coi,
    clock_step_us: clockStepUs(),
    build: threaded ? 'asyncify, threads' : 'asyncify, no threads',
    audio: page.audio ? (threaded ? 'AudioWorklet (threads)' : 'ScriptProcessorNode (no threads)') : 'none',
    service_worker: !!(navigator.serviceWorker && navigator.serviceWorker.controller),
    pointer: coarse ? 'coarse (touch)' : 'fine',
    device_pixel_ratio: window.devicePixelRatio || 1,
    viewport: window.innerWidth + 'x' + window.innerHeight,
    viewport_changes: 0,
    user_agent: navigator.userAgent,
  };

  // The page log. A print from the C frame loop must not touch layout:
  // rewriting the whole log and reading scrollHeight forced a layout inside
  // the frame, and each print took longer as the log grew. So lines wait in
  // a buffer and reach the DOM at most every 300 ms, from an idle callback,
  // as one appended text node; the box keeps the newest LOG_KEEP lines, its
  // column-reverse layout shows the newest with no scroll position read or
  // set, and style.css gives it `contain: strict`, so its layout cannot
  // reach the canvas. The console gets the same batch.
  const LOG_KEEP = 400;       // lines in the box; the session log keeps all
  const LOG_EVERY_MS = 300;   // at most one flush this often
  const logLines = [];        // every line, for sessionText()
  let pending = [], flushTimer = 0, shown = 0;
  const nodeLines = [];       // lines in each text node of the box, oldest first

  function flush() {
    flushTimer = 0;
    if (!pending.length) return;
    const text = pending.join('\n') + '\n';
    const n = pending.length;
    pending = [];
    console.log(text.slice(0, -1));
    const box = $('log-text');
    if (!box) return;
    box.appendChild(document.createTextNode(text));
    nodeLines.push(n);
    shown += n;
    while (shown - nodeLines[0] >= LOG_KEEP && box.firstChild) {
      shown -= nodeLines.shift();
      box.removeChild(box.firstChild);
    }
  }

  function scheduleFlush() {
    if (flushTimer) return;
    flushTimer = setTimeout(() => {
      if (window.requestIdleCallback) requestIdleCallback(flush, { timeout: 500 });
      else flush();
    }, LOG_EVERY_MS);
  }

  function log(text, isErr) {
    const line = (isErr ? '! ' : '') + text;
    if (logLines.length < 100000) logLines.push(line);
    pending.push(line);
    if (isErr) console.error(text);
    scheduleFlush();
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

  // The stage's content box: the viewport less the safe areas (style.css
  // pads the stage with env(safe-area-inset-*)), in CSS pixels.
  function stageBox() {
    const s = $('stage'), cs = getComputedStyle(s);
    const w = s.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
    const h = s.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom);
    return [Math.max(64, Math.floor(w)), Math.max(64, Math.floor(h))];
  }

  // The canvas keeps the drawing buffer it opened with (a resize would
  // change every stimulus's size in pixels during a run); a rotation or a
  // fullscreen change scales its CSS box to fit, aspect kept. SDL maps
  // touches and the mouse through that box, so the targets stay right.
  let logical = null;
  function fit() {
    const c = $('canvas');
    if (!document.body.classList.contains('running') || !c) return;
    if (!logical) {
      const r = c.getBoundingClientRect();
      if (r.width < 1) return;
      logical = [r.width, r.height];
    }
    const [W, H] = stageBox();
    const k = Math.min(W / logical[0], H / logical[1]);
    c.style.setProperty('width', Math.floor(logical[0] * k) + 'px', 'important');
    c.style.setProperty('height', Math.floor(logical[1] * k) + 'px', 'important');
    placeTargets();
  }

  // The target labels follow the canvas, whose halves they name: over its
  // width, and under it where the stage has room (the gallery's canvas in
  // portrait), else inside its bottom edge. The margin and the text scale
  // with the canvas's shorter side, not fixed pixels: 8 px from the bottom
  // put the text at the edge of a phone's screen. The canvas is inside the
  // safe area, so the labels are too.
  const TARGET_MARGIN = 0.05;   // of the canvas's shorter side
  const TARGET_FONT = 0.045;    // the same, clamped to 12..24 px
  function placeTargets() {
    const s = $('stage'), c = $('canvas'), t = s && s.querySelector('.targets');
    if (!t || !c) return;
    const sr = s.getBoundingClientRect(), cr = c.getBoundingClientRect();
    if (cr.width < 1) return;
    const unit = Math.min(cr.width, cr.height);
    const margin = Math.max(6, Math.round(unit * TARGET_MARGIN));
    const font = Math.min(24, Math.max(12, Math.round(unit * TARGET_FONT)));
    const line = Math.ceil(font * 1.5);
    const below = sr.bottom - parseFloat(getComputedStyle(s).paddingBottom) - cr.bottom;
    t.style.left = (cr.left - sr.left) + 'px';
    t.style.width = cr.width + 'px';
    t.style.right = 'auto';
    t.style.fontSize = font + 'px';
    t.style.lineHeight = line + 'px';
    t.style.top = 'auto';
    t.style.bottom = (sr.bottom - cr.bottom + (below >= line + 2 * margin ? -(line + margin) : margin)) + 'px';
  }

  // The stage covers what the participant sees: the visual viewport, which
  // a phone's browser shrinks while it shows its toolbars (100vh does not;
  // 100dvh is the CSS fallback in style.css).
  function stageVars() {
    const vv = window.visualViewport, s = $('stage');
    if (!vv || !s) return;
    s.style.setProperty('--stage-h', vv.height + 'px');
    s.style.setProperty('--stage-top', vv.offsetTop + 'px');
  }
  function stageViewport() { stageVars(); fit(); }
  if (window.visualViewport) {
    visualViewport.addEventListener('resize', stageViewport);
    visualViewport.addEventListener('scroll', stageViewport);
  }
  function onViewport() {
    const v = window.innerWidth + 'x' + window.innerHeight;
    if (v === status.viewport) return;
    status.viewport = v;
    if (document.body.classList.contains('running')) {
      status.viewport_changes++;
      log('viewport ' + v + (screen.orientation ? ' ' + screen.orientation.type : '') + ' during the run');
    }
    fit();
  }
  window.addEventListener('resize', onViewport);
  window.addEventListener('orientationchange', () => setTimeout(onViewport, 200));
  document.addEventListener('fullscreenchange', onViewport);
  document.addEventListener('webkitfullscreenchange', onViewport);
  // iOS Safari zooms on a pinch even with touch-action: none on old
  // versions; its own gesture events are the way to stop that, during a run.
  document.addEventListener('gesturestart', (e) => { if (document.body.classList.contains('running')) e.preventDefault(); });

  function requestFullscreen(el) {
    const f = el.requestFullscreen || el.webkitRequestFullscreen;
    if (!f) return false;   // iPhone Safari: no element fullscreen
    try {
      const p = f.call(el);
      if (p && p.catch) p.catch(() => {});
    } catch (e) { return false; }
    return true;
  }

  // Fullscreen during a run. The browser leaves it on Esc, on a switch of
  // tab or window, or by its own rules, and a page may ask again only from a
  // user gesture. So after an exit the stage says so, and the next key or
  // tap asks again; that key or tap is the page's, not a response, and never
  // reaches the program. Each exit is logged and counted (fullscreen_lost).
  let wantFs = false, fsLost = false, fsUp = null;
  const fsElement = () => document.fullscreenElement || document.webkitFullscreenElement;
  const running = () => document.body.classList.contains('running');
  function onFsChange() {
    if (!wantFs || !running()) return;
    if (fsElement()) {
      if (fsLost) log('fullscreen again');
      fsLost = false;
      document.body.classList.remove('fs-lost');
    } else if (!fsLost) {
      fsLost = true;
      status.fullscreen_lost = (status.fullscreen_lost || 0) + 1;
      log('fullscreen ended or was refused during the run; the next key or tap asks again');
      document.body.classList.add('fs-lost');
    }
  }
  document.addEventListener('fullscreenchange', onFsChange);
  document.addEventListener('webkitfullscreenchange', onFsChange);
  document.addEventListener('fullscreenerror', onFsChange);   // refused: the same note
  // Window capture listeners, added before the program's, so they see the
  // event first. A key down and a mouse press give a user activation, a
  // touch only on its release: ask there, and hold back the matching release.
  function fsAgain(e) {
    if (fsUp && (e.type === 'keyup' || e.type === 'pointerup' || e.type === 'pointercancel')) {
      if (fsUp === (e.type === 'keyup' ? 'k' + e.code : 'p' + e.pointerId)) {
        fsUp = null;
        if (e.type === 'pointerup' && e.pointerType !== 'mouse') requestFullscreen($('stage'));
        e.stopImmediatePropagation();
        e.preventDefault();
      }
      return;
    }
    if (!fsLost || !wantFs || !running() || e.key === 'Escape') return;
    if (e.target && e.target.closest && e.target.closest('button')) return;   // Stop stays Stop
    e.stopImmediatePropagation();
    e.preventDefault();
    if (e.type === 'keydown') { if (!e.repeat) fsUp = 'k' + e.code; requestFullscreen($('stage')); }
    else { fsUp = 'p' + e.pointerId; if (e.pointerType === 'mouse') requestFullscreen($('stage')); }
  }
  for (const t of ['keydown', 'keyup', 'pointerdown', 'pointerup', 'pointercancel']) window.addEventListener(t, fsAgain, true);

  function start(opts) {
    const args = page.args ? page.args() : [];
    // Fullscreen and an AudioContext both need this tap or click: the
    // request and the program's start (its sound opens first) run inside it.
    if (opts && opts.fullscreen) {
      status.fullscreen = requestFullscreen($('stage')) ? 'asked' : 'not available';
      wantFs = status.fullscreen === 'asked';
      status.fullscreen_lost = 0;
      fsLost = false;
      fsUp = null;
      if (wantFs && $('stage') && !$('stage').querySelector('.fsnote')) {
        const n = document.createElement('div');
        n.className = 'fsnote';
        n.textContent = 'Fullscreen ended. Press a key or tap to go back to fullscreen.';
        $('stage').appendChild(n);
      }
    }
    document.body.classList.add('running', 'preparing');
    if ($('intro')) $('intro').classList.add('hidden');
    $('stage').classList.add('running');
    stageVars();
    if ($('stop')) $('stop').disabled = false;
    if (!page.fixedSize) {
      const [w, h] = stageBox();
      args.push('--size', w + 'x' + h);
    }
    logical = null;
    log('start ' + (page.name || '') + ' ' + args.join(' '));
    Module.callMain(args);
  }

  function stop() {
    if (Module._yscr_request_abort) Module._yscr_request_abort();
  }

  function finish() {
    flush();   /* the run is over: the last lines now */
    wantFs = false;
    fsLost = false;
    document.body.classList.remove('running', 'preparing', 'fs-lost');
    $('stage').classList.remove('running');
    const fe = document.fullscreenElement || document.webkitFullscreenElement;
    if (fe) (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    if ($('stop')) $('stop').disabled = true;
    if ($('results')) $('results').classList.remove('hidden');
  }

  window.ysp = { status, log, download, sessionText, parseCsv, table, start, stop, finish, showStatus, coarse };

  window.Module = Object.assign({
    canvas: $('canvas'),
    noInitialRun: true,
    print: (t) => log(t),
    printErr: (t) => log(t, true),
    onRuntimeInitialized() {
      const b = $('start');
      if (b) { b.disabled = false; b.textContent = b.dataset.ready || 'Start'; }
    },
    // The program made and primed its stimuli: show the canvas now.
    onYspReady() {
      document.body.classList.remove('preparing');
      document.body.classList.add('ready');
      fit();
    },
    onAbort(what) { log('aborted: ' + what, true); },
  }, window.Module || {});

  // On a touch screen, fullscreen is on unless the visitor turns it off.
  document.addEventListener('DOMContentLoaded', () => {
    const fs = $('fs');
    if (fs && coarse) fs.checked = true;
  });

  if (script) {
    const s = document.createElement('script');
    s.src = script;
    s.async = true;
    document.body ? document.body.appendChild(s) : document.head.appendChild(s);
  }

  document.addEventListener('DOMContentLoaded', showStatus);
  if (document.readyState !== 'loading') showStatus();
})();
