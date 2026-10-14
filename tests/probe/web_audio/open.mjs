// One open of the sound demo as a visitor makes it: a new browser with a
// new profile, the page, a real click on Start (a trusted event, so a user
// activation), then the result. Prints one line: RESULT <json>.
//   node open.mjs chrome|firefox URL [--headed] [--n TRIALS] [--secs S]
//                 [--exe PATH] [--events]
// No autoplay flag or preference is set: the browser's own policy applies.
import puppeteer from 'puppeteer-core';

const a = process.argv.slice(2);
const which = a[0], url = a[1];
const opt = (k, d) => { const i = a.indexOf(k); return i >= 0 ? a[i + 1] : d; };
const has = (k) => a.includes(k);
const exe = opt('--exe', which === 'firefox' ? 'C:/Program Files/Mozilla Firefox/firefox.exe'
                                             : 'C:/Program Files/Google/Chrome/Application/chrome.exe');
const browser = await puppeteer.launch({
  browser: which === 'firefox' ? 'firefox' : 'chrome',
  executablePath: exe,
  headless: !has('--headed'),
  defaultViewport: { width: 1280, height: 860 },
  args: which === 'firefox' ? [] : ['--ignore-gpu-blocklist', '--enable-unsafe-swiftshader'],
  protocolTimeout: 120000,
});
const out = { browser: which, headed: has('--headed'), url };
try {
  const page = await browser.newPage();
  await page.goto(url, { waitUntil: 'load' });
  await page.waitForSelector('#start:not([disabled])', { timeout: 30000 });
  await page.evaluate((k) => { document.getElementById('n').value = k; }, opt('--n', '4'));
  await page.click('#start');
  const t0 = Date.now(), secs = parseFloat(opt('--secs', '45'));
  while (Date.now() - t0 < secs * 1000) {
    if (await page.evaluate(() => !!(window.Module && Module.yspResult))) break;
    await new Promise((r) => setTimeout(r, 250));
  }
  Object.assign(out, await page.evaluate((events) => {
    const r = window.Module && Module.yspResult, P = window.__probe;
    const s = (window.ysp && ysp.sessionText()) || '';
    const o = { ua: (navigator.userAgent.match(/(Chrome|Firefox|Version)\/[0-9.]+/) || [''])[0], coi: crossOriginIsolated,
                done: !!r, ctx: P ? P.summary() : null, noaudio: (s.match(/no audio: [^\n]*/) || [null])[0] };
    if (r) {
      o.have_audio = r.have_audio; o.callbacks = r.callbacks; o.reports = r.reports; o.xruns = r.xruns;
      o.audio_ok = r.trials.filter((t) => t.audio_minus_plan_ms > -1e8).length; o.trials = r.trials.length;
      o.path = ((r.audio || '').match(/path=(\S+)/) || [null, null])[1];
      o.describe = r.audio;
    } else o.log_tail = s.slice(-800);
    if (events && P) o.events = P.ev;
    return o;
  }, has('--events')));
} catch (e) {
  out.exception = String(e).slice(0, 300);
}
await browser.close();
console.log('RESULT ' + JSON.stringify(out));
