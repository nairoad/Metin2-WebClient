// preload.js - a download bar in the corner and downloading the whole corpus in the background
// Loaded by site/client.html.
//
// Two phases:
//   1. THE MOST IMPORTANT - `corpus/order.txt` (the recorded order
//      of use: login + village); done by `m2w_prefetch_start`
//      in compat/webfs_web.cpp (a window in memory + Cache Storage); here we only
//      show the progress;
//   2. THE REST IN THE BACKGROUND - all the other chunks of the manifest, one by one,
//      into Cache Storage. From there the prefetch and the background fetches
//      of runtime.js read them without the network (the game's synchronous XHR
//      does NOT go through the Service Worker).
// The bar disappears when everything is on disk. `?nofetch=1`
// turns phase 2 off.
//
// The texts ON THE BAR are for the player and still Polish (the page language
// is a separate decision); names and console messages are English.
(function () {
    var q = new URLSearchParams(location.search);
    var noBackground = q.get('nofetch') === '1';

    // --- Service Worker -----------------------------------------------------
    if ('serviceWorker' in navigator) {
        navigator.serviceWorker.register('sw.js').then(function (r) {
            console.log('[sw] registered, scope ' + r.scope);
        }).catch(function (e) {
            console.warn('[sw] no Service Worker (' + e + ') - chunks will come from the network on every start');
        });
    }

    // --- the corner bar ----------------------------------------------------
    var bar = document.createElement('div');
    bar.id = 'm2w_download';
    bar.style.cssText = 'position:fixed;right:10px;bottom:10px;z-index:20;min-width:230px;padding:8px 10px;' +
        'background:rgba(16,13,10,.85);border:1px solid #3a2f22;border-radius:6px;color:#e8d9b8;' +
        'font:12px/1.4 system-ui,sans-serif;pointer-events:none;display:none';
    bar.innerHTML = '<div id="m2w_download_text">Game data</div>' +
        '<div style="margin-top:5px;height:6px;background:#2a2218;border-radius:3px;overflow:hidden">' +
        '<div id="m2w_download_fill" style="height:100%;width:0;background:#c9a24a"></div></div>' +
        '<div id="m2w_download_detail" style="margin-top:3px;color:#9a8f7c;font-size:11px"></div>';
    document.body.appendChild(bar);
    var elText = bar.querySelector('#m2w_download_text');
    var elFill = bar.querySelector('#m2w_download_fill');
    var elDetail = bar.querySelector('#m2w_download_detail');
    var hideAt = 0;

    /// Shows the bar with `text`, the fill `fraction` (0..1) and a smaller `detail` line.
    function show(text, fraction, detail) {
        bar.style.display = 'block';
        elText.textContent = text;
        elFill.style.width = Math.round(Math.max(0, Math.min(1, fraction)) * 100) + '%';
        elDetail.textContent = detail || '';
    }
    /// Hides the bar after `delay` ms, unless `show` or `hide` was called again meanwhile.
    function hide(delay) {
        hideAt = performance.now() + (delay || 0);
        setTimeout(function () { if (performance.now() >= hideAt) bar.style.display = 'none'; }, (delay || 0) + 20);
    }

    // --- phase 2: the rest of the corpus -----------------------------------
    // `m2w.download` (console): start/end in ms, `done` downloaded, `fromCache`
    // already on disk, `errors`, `total` chunks, `bytes` downloaded.
    var download = { start: 0, done: 0, fromCache: 0, total: 0, bytes: 0, errors: 0, end: 0 };
    var m2w = globalThis.m2w = globalThis.m2w || {};
    m2w.download = download;

    /// Download speed of phase 2 so far, in MB/s (0 in the first half second).
    function megabytesPerSecond() {
        var s = (performance.now() - download.start) / 1000;
        return s > 0.5 ? (download.bytes / 1048576 / s) : 0;
    }

    /// Phase 2: every chunk of the manifest not yet in Cache Storage is
    /// downloaded into it, one at a time, with a pause so the game keeps its
    /// bandwidth; the bar shows the progress.
    function backgroundPhase() {
        var list = (m2w.corpus && m2w.corpus.all) || [];
        var base = (m2w.corpus && m2w.corpus.url) || 'corpus/';
        if (!list.length || noBackground || !('caches' in globalThis)) { hide(1500); return; }
        download.total = list.length;
        download.start = performance.now();
        caches.open('m2w-corpus').then(function (c) {
            var i = 0;
            /// Takes the next chunk: from disk if there, else from the network into the cache.
            function next() {
                if (i >= list.length) {
                    download.end = performance.now();
                    show('Game data: all on disk', 1, download.total + ' chunks');
                    console.log('[corpus] background: ' + download.done + ' downloaded, ' + download.fromCache + ' from disk, ' +
                                download.errors + ' errors, ' + Math.round((download.end - download.start) / 1000) + ' s');
                    hide(4000);
                    return;
                }
                var sUrl = base + list[i++] + '.bin';
                c.match(sUrl).then(function (cached) {
                    if (cached) { ++download.fromCache; return; }
                    return fetch(sUrl).then(function (o) {
                        if (!o || !o.ok) throw new Error('HTTP ' + (o && o.status));
                        return o.arrayBuffer().then(function (b) {
                            download.bytes += b.byteLength;
                            return c.put(sUrl, new Response(b, { headers: { 'Content-Type': 'application/octet-stream' } }));
                        });
                    }).then(function () { ++download.done; });
                }).catch(function (e) { ++download.errors; }).then(function () {
                    var n = download.done + download.fromCache + download.errors;
                    if (download.done > 0 || (n % 20) === 0)
                        show('Game data (background): ' + n + ' / ' + download.total, n / download.total,
                             download.done ? (megabytesPerSecond().toFixed(1) + ' MB/s' + (download.errors ? ', errors ' + download.errors : '')) : 'checking the disk...');
                    // We do not race the game for bandwidth: a short pause between chunks.
                    setTimeout(next, download.done ? 150 : 0);
                });
            }
            next();
        });
    }

    // --- phase 1: prefetch progress from compat ----------------------------
    var backgroundStarted = false;
    var lastDone = -1, lastChange = 0;
    /// Polls `m2w.prefetch` every 500 ms: shows phase 1, and starts phase 2
    /// when it is done or has stalled for 10 s (or there is no order.txt).
    function watch() {
        var w = m2w.prefetch;
        // m2w_prefetch fields in English (runtime.js: total/done/fromStorage)
        if (w && w.total > 0) {
            if (w.done !== lastDone) { lastDone = w.done; lastChange = performance.now(); }
            // The prefetch waits for room in memory (a sliding window) and can
            // stall when the game has left the recorded path - after 10 s without progress
            // we move to the background, which covers the rest of the list anyway.
            var bStalled = performance.now() - lastChange > 10000;
            if (w.done < w.total && !bStalled)
                show('Essential data: ' + w.done + ' / ' + w.total, w.done / w.total,
                     (w.fromStorage ? w.fromStorage + ' from disk' : ''));
            else if (!backgroundStarted) {
                backgroundStarted = true;
                show('Essential data: ready', 1, '');
                setTimeout(backgroundPhase, 2000);
            }
        } else if (!w && performance.now() > 20000 && !backgroundStarted && m2w.corpus && m2w.corpus.all.length) {
            // no order.txt - straight to the background
            backgroundStarted = true;
            backgroundPhase();
        }
        if (!backgroundStarted || (download.start && !download.end)) setTimeout(watch, 500);
    }
    setTimeout(watch, 1000);
})();
