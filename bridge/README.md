# bridge - WebSocket ↔ TCP

Transparent WebSocket ↔ TCP bridge that lets the browser client reach the
unmodified game server. A browser cannot open TCP; the client opens a
WebSocket to this bridge and the bridge opens the TCP connection.

With the kit you do not configure it by hand: `python webclient.py serve`
writes `build/port/private/bridge.json` (tools/bridge_config.py) and starts
the bridge next to the page; `python webclient.py bridge` starts only the
bridge. The allowlist is every address of the game's own server list
(`serverInfo.py` of the built client data), the `[bridge] port_span` ports
(default 100) from each listed port up on the same hosts - the game cores
the server announces after the character is chosen - plus `[bridge] targets`
of `webclient.toml`; the allowed page origins are the local page plus
`[bridge] origins`.

## What this does and does not do

It relays raw bytes. It does **not** parse, reframe, decrypt, or compress the
game protocol, and it must never be changed to do so. Packets are identified by
a one-byte header whose length is resolved through a header→size map, and
payloads are encrypted after login — there is no self-describing length prefix
the proxy could use even if it wanted to.

## Running

```
npm ci                    # once: the WebSocket library pinned in package-lock.json
npm start                 # uses config.local.json if present, else config.default.json
npm start ./my-config.json
PROXY_CONFIG=./my-config.json npm start
```

Never commit `config.local.json`; it may contain real server addresses.

## Configuration

| Key | Default | Meaning |
| :--- | :--- | :--- |
| `listenHost` | `127.0.0.1` | Interface the WebSocket server binds to. |
| `listenPort` | `9000` | Port the browser connects to. |
| `targets` | — | Allowlist of `name -> {host, port}`. Required. |
| `defaultTarget` | `null` | Target used when the client sends none. |
| `maxConnections` | `200` | Concurrent sessions before new ones are refused. |
| `maxConnectionsPerAddress` | `0` | Sessions one player address may hold at once (a player uses 1-2); `0` = no limit. Needs the real player address - see `trustProxy`. |
| `trustProxy` | `true` | Take the player address from `X-Forwarded-For` - only when the connection comes from this machine (nginx). The address is logged with every event. |
| `idleTimeoutMs` | `0` | Close a session after this long with no traffic either way; `0` = never (a player standing still sends nothing). |
| `heartbeatMs` | `30000` | Ping every browser this often; a browser that did not answer the previous ping is gone and its session is closed. Also keeps proxies that drop quiet WebSockets (Cloudflare after ~100 s) from closing the game. `0` = off. |
| `tcpConnectTimeoutMs` | `5000` | Give up on an unresponsive game server. |
| `highWaterMarkBytes` | `1048576` | Buffered bytes before backpressure engages. |
| `allowedOrigins` | `null` | `null` allows any origin - never use that on a bridge a browser can reach. When set to an array of strings, a WebSocket handshake is rejected unless its `Origin` header exactly matches one of them. `config.default.json` allows only the local page (`http://127.0.0.1:8731`, `http://localhost:8731`); `tools/bridge_config.py` writes the list for your site. |

A request picks its target in one of two ways:

- by **address**: `ws://host:11496/to/<host>:<port>` - the form the WebAssembly
  client uses (emscripten's socket layer puts the address it connects to into
  the path). It is accepted ONLY when exactly that host:port is a declared
  target (host compared without letter case); any other address is closed
  with 4001 before a TCP connection is made;
- by **name**: `ws://host:9000/?target=game`.

Any other request path (`//to/...`, `/TO/...`, `/x/../to/...`) is refused -
the raw path is matched, not a normalised one. The client never gets to pick
an address that is not in `targets` - a bridge that connects wherever the URL
says would be an open TCP relay.

With `allowedOrigins` set (as `python webclient.py serve` does), a handshake
WITHOUT an `Origin` header - i.e. not from a browser page - is refused too.

## Client integration notes

Read this before writing `CWebSocketNetworkDevice`.

- Set `binaryType` to `arraybuffer`. Everything is binary; a text round-trip
  corrupts every byte ≥ 0x80.
- **WebSocket message boundaries are not packet boundaries.** The proxy splits
  and coalesces the TCP stream freely. Feed received bytes into the same ring
  buffer `CNetworkStream` already uses and let the existing header→size logic do
  the framing. Never assume one message is one packet.
- Encryption stays entirely client-side, exactly as in the native client. The
  proxy never sees plaintext and must never be given a key.

## Close codes

| Code | Meaning |
| :--- | :--- |
| 4001 | Requested target (name or address) is not in the allowlist. |
| 4002 | Could not connect to the game server (refused or timed out). |
| 4003 | Game server closed the connection. |
| 4004 | Connection limit reached. |
| 4005 | Idle timeout. |

## Tests

```
npm test
```

## Deployment note

`listenHost` defaults to `127.0.0.1`. Exposing the proxy publicly means putting
it behind TLS (`wss://`) at a reverse proxy — browsers refuse plain `ws://` from
an `https://` page.

Binding to `127.0.0.1` does **not** protect against malicious pages in the
operator's own browser: WebSocket connections are exempt from same-origin
policy, so any page the operator's browser visits — not just pages served by
this proxy or its operator — can open `ws://127.0.0.1:9000/` and drive a
session into the game server, using the `defaultTarget` if no `target` is
even given. If the proxy runs on a machine where the operator also browses
the web in the same browser, set `allowedOrigins` to the specific origin(s)
that are allowed to connect (e.g. the game client's own page) so unrelated
pages are rejected at the handshake.
