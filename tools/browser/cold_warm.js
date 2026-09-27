// cold_warm.js - where the game's corpus chunks come from, first start vs second start.
//
// Opens the client page twice in the SAME headless Edge profile (Cache Storage,
// the HTTP cache and the Service Worker survive between the runs), waits, and
// prints the chunk counters of each run: blocking (synchronous) downloads and
// where they came from (Service Worker / HTTP cache / network), how long they
// blocked the main thread, chunks the game got from page memory, background
// and prefetch reads from Cache Storage, and the memory budget.
//
// Usage: node cold_warm.js <url> <seconds per run> <profile directory> [one]
// A 5th argument runs only once (e.g. a warm run on an existing profile).
// The client logs in with the local autologin when a bridge is running - use
// `&bridge=ws://127.0.0.1:1` in the URL to measure without the game server.
const puppeteer = require('puppeteer-core');
const { execSync } = require('child_process');
const EDGE = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const URL = process.argv[2] || 'http://127.0.0.1:8731/client.html';
const SECONDS = parseInt(process.argv[3] || '75', 10);
const PROFILE = process.argv[4] || 'cold_warm_profile';
/// Runs INSIDE the page: the corpus counters and the resource timings of the
/// synchronous chunk downloads.
function counters() {
  const s = m2w.chunks;
  const xhr = performance.getEntriesByType('resource')
    .filter((e) => /corpus\/[0-9a-f]{32}\.bin/.test(e.name) && e.initiatorType === 'xmlhttprequest');
  return {
    syncFetches: m2w.corpus.fetchCount,
    syncBlockedMs: Math.round(xhr.reduce((a, e) => a + e.duration, 0)),
    syncViaServiceWorker: xhr.filter((e) => e.workerStart > 0).length,
    syncFromHttpCache: xhr.filter((e) => e.transferSize === 0).length,
    syncFromNetwork: xhr.filter((e) => e.transferSize > 0).length,
    fromMemory: s ? s.fromMemory : null,
    backgroundFromStorage: s ? s.fromStorage : null,
    backgroundFromNetwork: s ? s.fromNetwork : null,
    prefetch: m2w.prefetch ? { total: m2w.prefetch.total, done: m2w.prefetch.done, fromStorage: m2w.prefetch.fromStorage } : null,
    distinctChunksUsed: new Set(m2w.corpus.order).size,
    deviceMemory: navigator.deviceMemory,
    budgetMB: m2w.chunks ? m2w.chunks.budget / 1048576 : null,
    cacheEntries: null,
  };
}
/// Working set in MB of the whole browser process tree (Windows; all
/// msedge processes whose ancestor is `pid`) - the tab's real memory cost,
/// including ArrayBuffers outside the JS heap.
function treeMemoryMB(pid) {
  try {
    const out = execSync('powershell -NoProfile -Command "Get-CimInstance Win32_Process | ' +
      'Select-Object ProcessId,ParentProcessId,WorkingSetSize | ConvertTo-Json -Compress"').toString();
    const all = JSON.parse(out);
    const kids = new Map();
    for (const p of all) { if (!kids.has(p.ParentProcessId)) kids.set(p.ParentProcessId, []); kids.get(p.ParentProcessId).push(p); }
    let sum = 0; const stack = [pid];
    const self = all.find((p) => p.ProcessId === pid); if (self) sum += self.WorkingSetSize;
    while (stack.length) for (const c of kids.get(stack.pop()) || []) { sum += c.WorkingSetSize; stack.push(c.ProcessId); }
    return Math.round(sum / 1048576);
  } catch (e) { return null; }
}

/// One run: opens the page, waits SECONDS, prints the counters, closes the tab.
async function run(browser, label) {
  const tab = await browser.newPage();
  await tab.setViewport({ width: 800, height: 600 });
  await tab.goto(URL, { waitUntil: 'domcontentloaded', timeout: 120000 });
  await new Promise((r) => setTimeout(r, SECONDS * 1000));
  const c = await tab.evaluate(counters);
  c.cacheEntries = await tab.evaluate(async () => (await (await caches.open('m2w-corpus')).keys()).length);
  c.browserTreeMB = treeMemoryMB(browser.process().pid);
  console.log(label + ' ' + JSON.stringify(c));
  await tab.close();
}
(async () => {
  const browser = await puppeteer.launch({
    executablePath: EDGE, headless: 'new', userDataDir: PROFILE, protocolTimeout: 300000,
    args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
           '--enable-webgl', '--ignore-gpu-blocklist', '--no-sandbox', '--window-size=800,600'],
  });
  if (!process.argv[5]) await run(browser, 'first ');
  await run(browser, process.argv[5] ? 'run   ' : 'second');
  await browser.close();
})().catch((e) => { console.error('FAILED: ' + e.stack); process.exit(1); });
