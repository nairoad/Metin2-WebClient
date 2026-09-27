# Deployment — hosting the client on your own site

Polish version: [WDROZENIE.md](WDROZENIE.md).

The client is a static web page plus a WebSocket→TCP bridge. Nothing runs on
the server except that bridge and a file server.

## 1. Files to upload — `python webclient.py package` builds `dist/site/` with exactly this set

| file / directory | size | notes |
|---|---|---|
| `client.html` | 3 kB | our own page (`site/client.html`) |
| `client.js`, `client.wasm` | 0.5 + 11 MB | engine; addresses carry `?v=<mtime>` so a rebuild is never served stale |
| `client.data` | ~40 MB | startup package: Python scripts, UI, baked fonts, cursors |
| `preload.js`, `sw.js` | small | download progress overlay and the Service Worker |
| `corpus/` | ~2 GB | `manifest.bin`, `order.txt` and ~500 chunks `<hash>.bin` (content-addressed: SHA-256 of the content cut to 16 bytes) |

Do **not** upload `private/`, `stage/`, `obj/`, `lib/`, `data/`, `bake/`.

## 2. HTTP server

Plain static hosting works (nginx, Apache, Caddy). Recommended headers
(a complete nginx site with them: section 3, step 5):

```
corpus/*.bin          Cache-Control: public, max-age=31536000, immutable
corpus/manifest.bin   Cache-Control: no-cache
corpus/order.txt  Cache-Control: no-cache
client.html                   Cache-Control: no-cache
client.js / .wasm / .data     Cache-Control: public, max-age=31536000  (they are versioned by ?v=)
*.wasm                        Content-Type: application/wasm
```

Enable gzip/brotli for `.js`, `.data` and `.html` (chunks are already
compressed data — do not recompress `.bin`).

HTTPS is required for the Service Worker and for `wss://` (browsers refuse
plain `ws://` from an https page except to localhost).

Only inside your home network (e.g. `http://192.168.1.10/metin2/client.html`)
plain HTTP works too: the client then uses `ws://` and runs without the
Service Worker and Cache Storage (browsers allow them only on HTTPS), so
every visit downloads the game data from your server again - on a local
network that is fast. For players on the internet use HTTPS.

## 3. The bridge - how the game reaches your server

### What it does

The game talks to its server over plain TCP (the auth port, e.g. 11000, and
the channel ports, e.g. 13000). **A browser cannot open TCP connections** - it
can only open WebSockets. The bridge (`bridge/`, a small Node.js program)
sits in between: the client opens a WebSocket to it, the bridge opens the TCP
connection to the game server and passes the bytes both ways, unchanged.

```
player's browser
   |
   |  https://your.site/metin2/client.html          -> nginx serves the game files
   |  wss://your.site/to/<game address>:<port>      -> nginx passes it to the bridge
   v
nginx (your.site, port 443)
   |  location /to/  ->  http://127.0.0.1:11496
   v
bridge (127.0.0.1:11496, on the same machine)
   |  plain TCP, only to the addresses of YOUR server list
   v
game server (auth 11000, channels 13000...)
```

The client builds the bridge address by itself: a page opened from
`https://your.site` connects to `wss://your.site/to/...` - the same site and
port as the page, path `/to/`. So on the web server you only need nginx to
pass `/to/` to the bridge. (Locally, `python webclient.py serve` runs the
bridge for you at `127.0.0.1:11496`, where the client looks when the page is
on localhost.)

The bridge is **not an open relay**: it connects only to the addresses of
your game's server list (read from the built client data) plus the
`[bridge] port_span` ports above each - the game cores the server sends the
client to after the character is chosen - plus `[bridge] targets` of
`webclient.toml`; and it accepts connections only from your page's origin.
**Nothing but the game may listen on those ports of the game server's host**
(e.g. 11000-11099, 13000-13129 for the default span): the bridge would let
the internet reach it. Lower `[bridge] port_span` to what your cores use.

### Step by step (a Linux server with nginx)

**1. Make the bridge configuration** - on the PC where you built the client,
with your site's address (scheme and host, no path):

```
python tools/bridge_config.py --site https://your.site
```

If players open the game under more than one address (e.g. the domain and,
at home, the web server's LAN address), repeat `--site` for each:
`--site https://your.site --site http://192.168.1.10`. A page whose address
is not listed gets `401` from the bridge and the game cannot connect.

It writes `build/port/private/bridge-site.json`. The file holds your game
server address - keep it private, do not commit it.

**2. Copy the bridge to the server** - the `bridge/` folder of this
repository (without `node_modules`) and `bridge-site.json`, e.g. into
`/opt/metin2-bridge/`.

**3. Install and try it** - on the server, with Node.js 20 or newer:

```
cd /opt/metin2-bridge
npm ci
node src/index.js bridge-site.json
```

It prints one line per event, starting with `{"event":"listening",...}`.
Stop it with Ctrl+C.

**4. Keep it running** - as a systemd service,
`/etc/systemd/system/metin2-bridge.service`:

```
[Unit]
Description=Metin2 WebClient bridge (WebSocket -> TCP)
After=network.target

[Service]
WorkingDirectory=/opt/metin2-bridge
ExecStart=/usr/bin/node src/index.js bridge-site.json
Restart=always
RestartSec=3
User=www-data
# hardening: the bridge only reads its folder and opens network connections
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX

[Install]
WantedBy=multi-user.target
```

```
sudo systemctl enable --now metin2-bridge
journalctl -u metin2-bridge -f        # its log
```

**5. nginx** - put the game files (the CONTENT of `dist/site/` from
`python webclient.py package`: `client.html`, `client.js`, `client.wasm`,
`client.data`, `preload.js`, `sw.js`, `corpus/`) into `/var/www/metin2/`,
then give nginx this site. On Debian/Ubuntu save it as
`/etc/nginx/sites-available/metin2` and enable it with
`sudo ln -s /etc/nginx/sites-available/metin2 /etc/nginx/sites-enabled/`;
elsewhere save it as `/etc/nginx/conf.d/metin2.conf`.

```
server {
    listen 443 ssl;
    server_name your.site;
    ssl_certificate     /etc/letsencrypt/live/your.site/fullchain.pem;   # e.g. from certbot
    ssl_certificate_key /etc/letsencrypt/live/your.site/privkey.pem;

    # the game files: /metin2/... -> /var/www/metin2/...
    # "no-cache" = the browser asks every time whether the file changed and
    # gets a short "not modified" when it did not - so an update reaches
    # players at once (client.html, sw.js, preload.js, corpus/manifest.bin).
    location /metin2/ {
        root /var/www;
        add_header Cache-Control "no-cache";
        add_header X-Content-Type-Options "nosniff" always;
        add_header Content-Security-Policy "frame-ancestors 'self'" always;
    }

    # the engine: client.html asks for it as client.js?v=<build time>, so a
    # new build is a new address - the browser may keep each one for a year
    location ~ ^/metin2/client\.(js|wasm|data)$ {
        root /var/www;
        add_header Cache-Control "public, max-age=31536000";
        add_header X-Content-Type-Options "nosniff" always;
        add_header Content-Security-Policy "frame-ancestors 'self'" always;
    }

    # the data chunks are named by the hash of their content, so a chunk
    # never changes - the browser may keep it for a year
    location ~ ^/metin2/corpus/[0-9a-f]+\.bin$ {
        root /var/www;
        add_header Cache-Control "public, max-age=31536000, immutable";
        add_header X-Content-Type-Options "nosniff" always;
        add_header Content-Security-Policy "frame-ancestors 'self'" always;
    }

    # the bridge: every /to/... WebSocket goes to it
    location /to/ {
        proxy_pass         http://127.0.0.1:11496;
        proxy_http_version 1.1;
        proxy_set_header   Upgrade $http_upgrade;
        proxy_set_header   Connection "upgrade";
        proxy_set_header   Host $host;
        proxy_set_header   X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_read_timeout 3600s;
        proxy_send_timeout 3600s;
    }
}
```

Then `sudo nginx -t && sudo systemctl reload nginx`.

Why the extra lines: `nosniff` stops browsers from guessing file types, and
`frame-ancestors 'self'` stops other sites from showing your game inside a
frame (clicks tricked through an invisible frame). nginx drops headers set
in the `server` block for every `location` that sets its own, which is why
they are repeated in each. With HTTPS working for good, also consider
`Strict-Transport-Security` (certbot can add it). Keep `bridge-site.json`
readable only by the service: `sudo chown root:www-data bridge-site.json &&
sudo chmod 640 bridge-site.json` - it holds your game server address.

**Optional - limit connections per player address.** The bridge logs the
player's address (from `X-Forwarded-For`, which nginx sets above) and can
refuse more than N sessions from one address: `[bridge] max_per_address =
20` in `webclient.toml`, then make the configuration again (step 1). Only
with the REAL player address: behind Cloudflare nginx sees Cloudflare's
addresses, so first add to the `server` block `real_ip_header
CF-Connecting-IP;` and a `set_real_ip_from <range>;` line for every range
listed at https://www.cloudflare.com/ips/ - otherwise all players share a
few addresses and the limit would lock them out.

- **Only in your home network, without a certificate:** replace the first
  four lines with `listen 80;` and `server_name 192.168.1.10;` (the web
  server's address) and open `http://192.168.1.10/metin2/client.html`.
- **The server already has a site on the same port and name** (e.g. the
  `default` site on port 80): do not add a second `server` block - copy
  the four `location` blocks into the existing one, or disable the other
  site (`sudo rm /etc/nginx/sites-enabled/default` removes only the link).
- **Leftovers of an earlier setup:** other `location` blocks with a regular
  expression for `.bin` / `.wasm` (e.g. `location ~* \.(bin|wasm)$`) win
  over `location /metin2/` - remove them (step 6 explains why).
- **Behind Cloudflare:** keep WebSockets on (Network settings, on by
  default) and SSL mode "Full". The bridge pings every browser every 30 s,
  so Cloudflare does not drop a quiet game.
- If `client.wasm` does not come with `Content-Type: application/wasm`
  (an old nginx), add the line `application/wasm wasm;` inside the `types`
  list of `/etc/nginx/mime.types` (not a `types` block in the site - that
  would replace the whole list and break the other files).

**6. Check it**

- `curl -i https://your.site/to/ping` answers `426 Upgrade Required` - nginx
  reaches the bridge (a plain request is not a WebSocket, hence 426);
- open `https://your.site/metin2/client.html` and log in; the bridge log
  shows `session_opened` and `upstream_connected`;
- `connection_rejected ... unknown_target` in the log means the game asked
  for an address the bridge does not allow - add it to `[bridge] targets`
  in `webclient.toml`, make the configuration again (step 1) and restart the
  bridge.
- `curl -I https://your.site/metin2/client.wasm` answers `200` with
  `Content-Type: application/wasm`. A `404` usually means one of:
  the server has the repository's `site/` folder instead of the content of
  `dist/site/` (there must be `client.js`, `client.wasm`, `client.data` and
  `corpus/`); nginx looks in another folder (`root /var/www;` means the
  files must be in `/var/www/metin2/`); or an older REGEX location such as `location ~* \.(bin|wasm)$`
  takes the request - regex locations win over `location /metin2/`. Remove
  such leftovers: they also mark `corpus/manifest.bin` as immutable, and
  after an update players would keep the old one.
- The bridge stops with `EADDRINUSE ... 127.0.0.1:11496`: another program
  (e.g. an earlier bridge running as a service) holds the port.
  `ss -ltnp | grep 11496` names it; stop and disable its service.

**When the web server is outside the game server's network**

The bridge dials the game server **from the web server**, using the server
address built into the client (`[server] address` in `webclient.toml`,
and the addresses in the client's `serverInfo.py`). The player's browser
never connects to the game server itself. So:

- web server in the same network as the game server (e.g. both at home):
  a private address such as `192.168.x.x` in the client is fine;
- web server elsewhere (a VPS, a hosting): build the client with an address
  the WEB SERVER can reach - usually the game server's public IP or domain -
  and open the auth and channel ports to the web server;
- after the character is chosen, the game server sends the client to the
  address it is configured with (in the server's own config). If that host
  differs from the one in the client, add it to `[bridge] targets` and make
  the configuration again - otherwise the bridge log shows `unknown_target`.

`bridge_config.py --site` warns when the page is on a public host while the
game server addresses are private ones.

**Notes**

- The bridge machine must be able to reach the game server ports (firewall).
- On a Windows server the same works with nginx for Windows; keep the bridge
  running with the Task Scheduler ("at startup") or a service wrapper such
  as NSSM.
- The game server address inside the client comes from the build
  (`[server] address` in `webclient.toml`).

## 4. Bandwidth and the first start

The streamed corpus is ~2 GB; the recorded startup path (login + village)
is 97 chunks ≈ 400 MB. Order of events on a cold browser:

1. `client.js/.wasm/.data` (~50 MB) — the page shows engine progress.
2. `order.txt` prefetch: chunks in the order the game will ask for
   them, into memory (a window of 96-512 MB, by device memory) and Cache
   Storage. The overlay in the
   corner shows "Essential data".
3. The game starts and asks for chunks synchronously; whatever the
   prefetch has not reached yet comes from the network (one chunk at a
   time, the frame waits — on a 15 Mbit/s uplink a 4 MB chunk is ~2 s).
4. Background: the remaining chunks download one by one into Cache
   Storage ("Game data (background): N / total"); a 150 ms pause between chunks
   leaves bandwidth for the game.
5. On the next start the prefetch reads the startup path from Cache Storage
   (disk) into memory, ahead of the game. What the game still asks for
   synchronously (measured: 22 chunks with a 512 MB budget, 138 with 96 MB)
   comes from the browser's HTTP cache when the server sends
   `Cache-Control: public, max-age=31536000, immutable` on `corpus/*.bin`
   (measured: 21 of 22 from the HTTP cache), otherwise from the network.
   The synchronous requests do NOT go through the Service Worker - Chromium
   does not route synchronous XHR to it; the chunk memory budget
   (`?corpusMB=`, default by device memory) is what matters most.

If the Service Worker cannot register (http, or a browser that blocks it),
the game still works; Cache Storage itself needs only a secure origin
(https or localhost), so the prefetch from disk works without the worker.

Rough numbers for a 15 Mbit/s uplink (≈1.9 MB/s): engine ~30 s, startup
path ~3.5 min, whole corpus ~18 min in the background — once per browser.

## 5. The "Play in browser" page

Open the client in a **new tab** from your site:

```
<a href="https://your.site/metin2/client.html?deflang=pl&scale=1.5&return=https://your.site" target="_blank">
```

- `scale=` — UI scale chosen by the player (1, 1.25, 1.5, 1.75, 2, 2.5, 3);
  remembered in localStorage, so it can be set once from a settings form.
- `return=` — where "Exit game" navigates when the tab was not opened by
  script and cannot close itself.
- `deflang=` — client language.
- volume: not yet exposed as a parameter (the in-game options dialog works;
  a URL parameter is on the list).

## 6. Checklist

- [ ] HTTPS on the site, `wss://` reaches the bridge (`curl -i https://your.site/to/ping` answers `426 Upgrade Required`)
- [ ] `corpus/manifest.bin` and one chunk download with the headers above
- [ ] F12 console shows `[sw] registered` and later `[corpus] background: ... downloaded`
- [ ] second start: `m2w.corpus.fetchCount` small, chunks served in milliseconds
