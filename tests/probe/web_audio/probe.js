// Silence probe for the sound demo (docs/web.md, "How to check the sound
// at open"). serve.py puts it first in a page. It wraps AudioContext and
// records each context's life: its state over time, each resume(),
// suspend() and close() with the page's user activation at the call
// (T transient, S sticky), and the ScriptProcessorNode's callbacks. Every
// connection to a destination is also tapped by an AnalyserNode, which a
// timer reads every 100 ms: a read with a sample above 0.005 is sound.
// window.__probe.summary() gives one record per context.
(function () {
  'use strict';
  const P = window.__probe = { ev: [], ctx: [] };
  const now = () => Math.round(performance.now() * 10) / 10;
  const ua = navigator.userActivation;
  const act = () => ua ? (ua.isActive ? 'T' : '-') + (ua.hasBeenActive ? 'S' : '-') : '??';
  const log = (s) => { if (P.ev.length < 400) P.ev.push(now() + ' ' + s + ' [' + act() + ']'); };
  for (const t of ['pointerdown', 'click', 'touchend'])
    addEventListener(t, (e) => log(t + ' trusted=' + e.isTrusted), true);
  document.addEventListener('visibilitychange', () => log('visibility ' + document.visibilityState));
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return;
  const connect = AudioNode.prototype.connect;
  AudioNode.prototype.connect = function (dst) {
    const r = connect.apply(this, arguments);
    try {
      if (dst instanceof AudioDestinationNode) {
        const rec = P.ctx.find((x) => x.c === this.context);
        if (rec) {
          if (!rec.an) { rec.an = this.context.createAnalyser(); rec.an.fftSize = 32768; rec.buf = new Float32Array(32768); }
          connect.call(this, rec.an);
          log('ctx' + rec.id + ' tap on ' + this.constructor.name);
        }
      }
    } catch (e) { log('tap error ' + e); }
    return r;
  };
  function Wrapped(opts) {
    const c = opts === undefined ? new AC() : new AC(opts);
    const id = P.ctx.length;
    const rec = { id, c, t_new: now(), rate: c.sampleRate, states: [[now(), c.state]], procs: 0, first_proc: null,
                  polls: 0, loud: 0, first_loud: null, max: 0, t_running: c.state === 'running' ? now() : null, closed: false };
    P.ctx.push(rec);
    log('ctx' + id + ' new state=' + c.state + ' rate=' + c.sampleRate + ' opts=' + JSON.stringify(opts || {}));
    c.addEventListener('statechange', () => {
      rec.states.push([now(), c.state]);
      if (c.state === 'running' && rec.t_running === null) rec.t_running = now();
      log('ctx' + id + ' state=' + c.state + ' t=' + c.currentTime.toFixed(3));
    });
    for (const m of ['resume', 'suspend', 'close']) {
      const f = c[m].bind(c);
      c[m] = function () {
        log('ctx' + id + ' ' + m + '() state=' + c.state);
        if (m === 'close') rec.closed = true;
        const p = f();
        p.then(() => log('ctx' + id + ' ' + m + ' resolved state=' + c.state),
               (e) => log('ctx' + id + ' ' + m + ' rejected ' + e));
        return p;
      };
    }
    const csp = c.createScriptProcessor && c.createScriptProcessor.bind(c);
    if (csp) c.createScriptProcessor = function () {
      const n = csp.apply(null, arguments);
      n.addEventListener('audioprocess', () => { rec.procs++; if (rec.first_proc === null) { rec.first_proc = now(); log('ctx' + id + ' first audioprocess'); } });
      return n;
    };
    const timer = setInterval(() => {
      if (rec.closed && c.state === 'closed') { clearInterval(timer); return; }
      if (!rec.an || c.state !== 'running') return;
      rec.an.getFloatTimeDomainData(rec.buf);
      let m = 0;
      for (let i = 0; i < rec.buf.length; i++) { const v = Math.abs(rec.buf[i]); if (v > m) m = v; }
      rec.polls++;
      if (m > rec.max) rec.max = m;
      if (m > 0.005) { rec.loud++; if (rec.first_loud === null) { rec.first_loud = now(); log('ctx' + id + ' first sound peak=' + m.toFixed(4)); } }
    }, 100);
    return c;
  }
  Wrapped.prototype = AC.prototype;
  window.AudioContext = Wrapped;
  if (window.webkitAudioContext) window.webkitAudioContext = Wrapped;
  P.summary = function () {
    return P.ctx.map((r) => ({ id: r.id, t_new: r.t_new, rate: r.rate, state: r.c.state,
      states: r.states.map((s) => s[0] + ':' + s[1]).join(' '), t_running: r.t_running, procs: r.procs,
      first_proc: r.first_proc, polls: r.polls, loud: r.loud, first_loud: r.first_loud, max: +r.max.toFixed(4) }));
  };
})();
