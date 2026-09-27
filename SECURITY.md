# Security

## Reporting a vulnerability

Please do **not** open a public issue for a security problem. Use GitHub's
private reporting instead: the **Security** tab of this repository ->
**Report a vulnerability**. Describe what an attacker can do, how to
reproduce it, and which version (commit) you tested.

Only the current `main` branch is maintained.

## What is in scope

- **The web client** that runs in players' browsers (`site/`,
  `compat/runtime.js` and the rest of `compat/`) - e.g. anything a link to
  an operator's real site can make the page do.
- **The bridge** (`bridge/`) that operators expose to the internet - e.g.
  reaching anything other than the game server, or taking it down cheaply.
- **The build tools** (`tools/`, `webclient.py`) that run on the operator's
  machine with their game files - e.g. a crafted pack or model file writing
  outside the build directory.

The game engine itself (the TMP4 sources you build with) and the game
server are not part of this project.

## Running it safely

- Serve the site over HTTPS and use the nginx site and the systemd unit of
  [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) as they are (security headers,
  the bridge on 127.0.0.1 only, a hardened service).
- `bridge-site.json` holds your game server address - keep it readable
  only by the bridge service, and never commit `webclient.toml` or
  `build/port/private/`.
- On a real site the page accepts only the player options of the address
  (scale, language, return address, cursor, limits); the bridge and data
  addresses come from your build, not from links.
- The bridge connects only to your game's server list plus
  `[bridge] port_span` ports above each - make sure nothing but the game
  listens on those ports of the game server's host.
