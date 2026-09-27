// measure.js - runs the client in a windowless browser and returns the log.
//
// WHY: so that the loop "run -> read where it stopped -> fix"
// works FROM THE COMMAND LINE, not through a human clicking in a tab.
//
// Edge is Chromium, so puppeteer-core drives it the same way as Chrome.
// I do not download a browser of my own - this one is already on disk.
//
// WebGL IN HEADLESS MODE: there is no graphics card there, so Chromium
// draws in software (SwiftShader). It has to be asked for that explicitly, otherwise
// `getContext("webgl2")` returns null and the client ends at creating the device.
//
// ---------------------------------------------------------------------------
// WHY THE READ IS IN A LOOP AND NOT AT THE END
// ---------------------------------------------------------------------------
// The first version waited 45 seconds and only then read the log. It worked
// as long as the client FINISHED. When it stopped finishing, it stopped working and gave
// NOTHING: `Runtime.callFunctionOn` waits for the page's main thread, and that was
// busy - so the measurement ended with a timeout message and zero
// knowledge of how far the client got.
//
// Now I read EVERY HALF SECOND and remember the LAST SUCCESSFUL read. When the page
// stops responding, I have the log from the moment right before that - i.e. exactly
// what I want to know: what was the last thing the client managed to do.
//
// It is the same rule as with `Traceback()`: a tool is to
// say what happened also when - or rather ESPECIALLY when - things
// did not go well.

const puppeteer = require('puppeteer-core');

const EDGE = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const URL = process.argv[2] ||
      'http://127.0.0.1:8731/client.html?run=1&missing=1';
const WAIT_MS = parseInt(process.argv[3] || '60000', 10);
const STEP_MS = 500;
// Window size from the command line. In headless mode SwiftShader draws,
// i.e. the processor - and the cost of a frame grows with the number of pixels. A small window
// decides whether the page is BLOCKED or just slow.
const WIDTH = parseInt(process.env.M2W_WIDTH || '400', 10);
const HEIGHT = parseInt(process.env.M2W_HEIGHT || '300', 10);

/// Runs INSIDE the page: WebGL presence, the measuring shell's output text,
/// `syserr.txt` and `/missing.txt` from the wasm file system, the last error.
function readFromPage() {
  /// Text of the file `name` in the wasm file system, or null when it is absent.
  const readFile = (name) => {
    try {
      return new TextDecoder('utf-8').decode(Module.FS.readFile(name));
    } catch (e) {
      return null;
    }
  };
  return {
    // I DO NOT CREATE A NEW CANVAS HERE.
    //
    // The first version did `createElement("canvas")` and asked it
    // for a context - every half second, for the whole measurement. The browser has
    // a HARD LIMIT of active WebGL contexts and after exceeding it
    // takes away the OLDEST. The oldest was the client's context.
    //
    // So the measuring tool killed what it measured, and showed
    // an empty screen as the result. So I ask for the canvas that already exists.
    webgl: (() => {
      try {
        const c = document.getElementById('canvas') ||
                  document.querySelector('canvas');
        return !!(c && (c.getContext('webgl2') || c.getContext('webgl')));
      } catch (e) { return false; }
    })(),
    page: (document.getElementById('log') || {}).textContent || '',
    syserr: readFile('syserr.txt'),
    missing: readFile('/missing.txt'),
    error: globalThis.m2w_last_error || null,
  };
}

(async () => {
  const browser = await puppeteer.launch({
    protocolTimeout: 180000,
    executablePath: EDGE,
    headless: 'new',
    args: [
      '--use-gl=angle',
      '--use-angle=swiftshader',
      '--enable-unsafe-swiftshader',
      '--enable-webgl',
      '--ignore-gpu-blocklist',
      '--no-sandbox',
      '--window-size=' + WIDTH + ',' + HEIGHT,
    ],
  });

  const tab = await browser.newPage();
  await tab.setViewport({ width: WIDTH, height: HEIGHT });

  tab.on('console', (m) => console.log('[console] ' + m.text()));
  tab.on('pageerror', (e) => console.log('[exception] ' + e.message));
  tab.on('requestfailed', (r) =>
    console.log('[not fetched] ' + r.url() + ' ' + r.failure().errorText));

  console.log('opening: ' + URL);
  await tab.goto(URL, { waitUntil: 'domcontentloaded', timeout: 120000 });

  let last = null;
  let reads = 0;
  let blocked = false;
  const end = Date.now() + WAIT_MS;

  while (Date.now() < end) {
    try {
      const timer = new Promise((_, reject) =>
        setTimeout(() => reject(new Error('no answer')), 60000));
      last = await Promise.race([tab.evaluate(readFromPage), timer]);
      reads += 1;
    } catch (e) {
      blocked = true;
      break;
    }
    await new Promise((r) => setTimeout(r, STEP_MS));
  }

  console.log('successful reads: ' + reads +
              (blocked ? '  (then the page stopped responding)' : ''));

  try { await tab.screenshot({ path: 'screenshot.png' }); }
  catch (e) { console.log('screenshot failed: ' + String(e.message).slice(0, 120)); }

  if (last) {
    console.log('\n=== WebGL: ' + (last.webgl ? 'YES' : 'NO') + ' ===');
    console.log('\n=== PAGE ===');
    console.log(last.page.slice(-8000));
    if (last.syserr !== null) {
      console.log('\n=== syserr.txt ===');
      console.log(last.syserr);
    } else {
      console.log('\n=== syserr.txt: NONE (the client did not create it) ===');
    }
    if (last.missing) {
      console.log('\n=== missing.txt ===');
      console.log(last.missing);
    }
    if (last.error) {
      console.log('\n=== REJECTED PROMISE ===');
      console.log(last.error);
    }
  } else {
    console.log('\nNOT A SINGLE READ SUCCEEDED');
  }

  await browser.close();
})().catch((e) => { console.error('MEASUREMENT FAILED: ' + e.stack); process.exit(1); });
