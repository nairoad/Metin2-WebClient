// sw.js - the client's Service Worker.
//
// One task: serve corpus chunks (`corpus/<hash>.bin`)
// from Cache Storage, and what is not there - download, STORE and serve.
//
// Why this is needed: the game reads chunks with a SYNCHRONOUS XHR on
// the main thread (a deliberate decision, `compat/webfs_web.cpp`). A synchronous
// XHR cannot read Cache Storage, and the HTTP cache turned out to be
// unreliable.
//
// CORRECTION (measured): the earlier assumption
// that a Service Worker intercepts synchronous XHR too is FALSE in
// Chromium - a synchronous XHR goes past this worker straight to the network
// (workerStart 0, full transfer) even for a chunk that is in the cache. The
// worker still serves the ASYNCHRONOUS reads (the prefetch and the background
// fetches of runtime.js, which also read Cache Storage directly). What
// actually spares the game blocking downloads is the chunk memory budget
// (runtime.js m2w.corpusBudget).
//
// Requires a secure origin (https or localhost). Nothing but
// chunks is intercepted - `client.js/.wasm/.data` have their own
// versioning (`?v=`).

var CACHE_NAME = 'm2w-corpus';

// Caches of earlier layouts, deleted at activation (none so far; a renamed
// cache goes here so players do not keep two copies of the data).
var OLD_CACHES = [];

self.addEventListener('install', function () { self.skipWaiting(); });
self.addEventListener('activate', function (e) {
    e.waitUntil(Promise.all(OLD_CACHES.map(function (name) { return caches.delete(name); }))
        .then(function () { return self.clients.claim(); }));
});

self.addEventListener('fetch', function (e) {
    var url = e.request.url;
    if (e.request.method !== 'GET') return;
    if (url.indexOf('/corpus/') < 0 || !/[0-9a-f]{32}[.]bin([?].*)?$/.test(url)) return;
    e.respondWith(
        caches.open(CACHE_NAME).then(function (cache) {
            return cache.match(url).then(function (hit) {
                if (hit) return hit;
                return fetch(e.request).then(function (response) {
                    if (response && response.ok) {
                        try { cache.put(url, response.clone()); } catch (err) { }
                    }
                    return response;
                });
            });
        })
    );
});
