// stack.js - asks a blocked page WHERE exactly it stands.
//
// WHY
// ===
// `measure.js` says the client stops at the first turn of the main loop.
// `Process()` has a dozen calls and each of them could loop.
// Guessing which one is exactly the kind of work this project
// is not supposed to do.
//
// The browser's debugger can PAUSE execution and return the call stack,
// also in the middle of the wasm. The client is linked with `-g2`, so the stack
// has FUNCTION NAMES, not `wasm-function[30218]`.
//
// This is the same tool as `Traceback()` on the Python side: the question
// "where are you", asked at the moment when nothing else answers any more.

const puppeteer = require('puppeteer-core');

const EDGE = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const URL = process.argv[2] ||
      'http://127.0.0.1:8731/client.html?run=1';
const BEFORE_PAUSE_MS = parseInt(process.argv[3] || '25000', 10);

(async () => {
  const browser = await puppeteer.launch({
    protocolTimeout: 120000,
    executablePath: EDGE,
    headless: 'new',
    args: [
      '--use-gl=angle',
      '--use-angle=swiftshader',
      '--enable-unsafe-swiftshader',
      '--no-sandbox',
      '--window-size=400,300',
    ],
  });

  const tab = await browser.newPage();
  await tab.setViewport({ width: 400, height: 300 });
  tab.on('console', (m) => console.log('[console] ' + m.text()));

  const cdp = await tab.createCDPSession();
  await cdp.send('Debugger.enable');

  const paused = new Promise((r) => cdp.once('Debugger.paused', r));

  console.log('opening: ' + URL);
  await tab.goto(URL, { waitUntil: 'domcontentloaded', timeout: 120000 });

  await new Promise((r) => setTimeout(r, BEFORE_PAUSE_MS));
  console.log('pausing execution...');
  cdp.send('Debugger.pause').catch(() => {});

  const timer = new Promise((_, reject) =>
    setTimeout(() => reject(new Error('the debugger did not pause')), 60000));

  try {
    const event = await Promise.race([paused, timer]);
    console.log('\n=== CALL STACK (from the top) ===');
    for (const frame of event.callFrames.slice(0, 60)) {
      const file = (frame.url || '').split('/').pop();
      console.log('  ' + (frame.functionName || '(no name)') +
                  '   [' + file + ':' + (frame.location.lineNumber + 1) + ']');
    }
    console.log('  ...frames in total: ' + event.callFrames.length);
  } catch (e) {
    console.log('COULD NOT PAUSE: ' + e.message);
  }

  await browser.close();
})().catch((e) => { console.error('FAILED: ' + e.stack); process.exit(1); });
