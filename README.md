# Metin2 WebClient

[![tests](https://github.com/nairoad/Metin2-WebClient/actions/workflows/tests.yml/badge.svg)](https://github.com/nairoad/Metin2-WebClient/actions/workflows/tests.yml)

**Polska wersja: [README.pl.md](README.pl.md).**

Play your Metin2 server in a web browser. This kit takes **your** client
sources and **your** game files and builds a browser version of the game
client (`client.html` + `client.wasm`), plus the small bridge a browser
needs to talk to your game server. Players open a web page and play - no
installation.

It works with the classic TMP4 ("mainline") client sources and is meant to
be adapted to other forks. What is playable today: login, character select,
the world, combat, mounts, skills, shops, quests, chat, friends, options.

> This repository contains only the kit (our code and tools). It does **not**
> contain game files, client sources or any third-party SDK - you bring your
> own.

---

## What you need

**A Windows 10/11 PC** (the build runs on Windows; other systems are not
tested), about **6 GB of free disk space**, and:

| you bring | what it is |
|---|---|
| your client **sources** | the C++ source folder of your client, the one with `source/` (GameLib, EterLib, UserInterface...) and `extern/` |
| your installed **game client** | the folder with `pack/` (the `.eix`/`.epk` files) - the same client your players use |
| your **server address** | the address players connect to |

Install these tools once:

| tool | how | why |
|---|---|---|
| [Python 3.13](https://www.python.org/downloads/) | installer, tick "Add to PATH" | runs the whole build (3.11/3.12 work too) |
| [Python 3.10](https://www.python.org/downloads/release/python-31011/) (or any 3.8-3.12) | installer, **no** need to add to PATH | rewrites the game's old Python 2 scripts (needs `lib2to3`, removed in 3.13) |
| [Git for Windows](https://git-scm.com/download/win) | installer | to download this project and the Emscripten SDK |
| [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) **6.0.8** | `git clone https://github.com/emscripten-core/emsdk.git C:\emsdk`, then in `C:\emsdk`: `emsdk install 6.0.8` and `emsdk activate 6.0.8` | the C++ -> WebAssembly compiler. Use 6.0.8: the build downloads a Python engine made with exactly this version. Elsewhere than `C:\emsdk`? set `[tools] emsdk` |
| Pillow | `pip install pillow` | converts the mouse cursors |
| [Node.js 20+](https://nodejs.org/) | installer (brings `npm`) | runs the bridge between the browser and your server |

Optional, for a better result:

| tool | gives you |
|---|---|
| [Visual Studio Build Tools 2022](https://visualstudio.microsoft.com/downloads/) with "Desktop development with C++" (x86) | trees on the maps, and ~1% of models that would otherwise be missing |
| Microsoft Edge | the automatic quality checks (`python webclient.py check`) |

## Quick start

**1. Get the kit**

```
git clone https://github.com/nairoad/Metin2-WebClient.git
cd Metin2-WebClient
```

**2. Tell it where your files are.** Copy `webclient.example.toml` to
`webclient.toml` and fill in three lines (use `/` in paths):

```toml
[inputs]
source = "C:/path/to/your/client-sources"
client = "C:/path/to/your/game-client"

[server]
address = "your.server.address"
```

If Python 3.10 is not in the default place, also set `python2to3` under
`[tools]` - every key is explained in `webclient.example.toml`.

**3. Check that everything is in place**

```
python webclient.py doctor
```

Every missing item is one line saying how to fix it. Repeat until it says
`ready to build`.

**4. Build** (the first build takes about 45 minutes - more than half of it a
one-time check of all models; later builds take a few minutes)

```
python webclient.py build
```

It stops at the first problem, says in one sentence what to do, and tells
you how to continue from that step.

**5. Play**

```
cd bridge
npm ci
cd ..
python webclient.py serve
```

(`npm ci` only the first time - it installs the bridge.) Open
**http://127.0.0.1:8731/client.html** and log in. Ctrl+C stops it.

## Put it on your website

`python webclient.py package` puts everything to upload into `dist/site/`
(about 2 GB, mostly game data). On your web server you also run the bridge
(`python tools/bridge_config.py --site https://your.site` makes its settings)
and let nginx pass `/to/` to it, so browsers reach the game over HTTPS. What
the bridge is and each step, with an nginx example:
[docs/DEPLOYMENT.md](docs/DEPLOYMENT.md#3-the-bridge---how-the-game-reaches-your-server).

## If something goes wrong

| what you see | what to do |
|---|---|
| `doctor` shows `FAIL` | follow the line under it; run `doctor` again |
| build stops at `stage` with `DID NOT HIT` | your client sources differ from TMP4 at one place the kit patches - see [docs/BUILDING.md](docs/BUILDING.md#4-when-a-step-fails) |
| build stops with `error:` in a file | a part of your sources the kit does not handle yet - the line names the file |
| the step `gr2` says the helper "cannot be started" | your antivirus removed a freshly built tool - allow the build folder, or skip it: `python webclient.py build --skip gr2` |
| the game waits at "connecting to the server" | the bridge is not running - use `python webclient.py serve` (or `python webclient.py bridge`) |
| you get logged out after choosing a character | the server sent the game to a port the bridge does not allow - add it: `[bridge] targets = ["address:port"]` in `webclient.toml` |
| text looks wrong, trees are missing | optional tools were missing during the build - see `doctor` (`windows-gdi`, `msvc`, `speedtree`) |

More: [docs/BUILDING.md](docs/BUILDING.md) (every step and every tool).

## URL options

Add to the page address, e.g. `client.html?scale=1.5`:

| option | meaning |
|---|---|
| `scale=1..3` | interface size (1, 1.25, 1.5, 1.75, 2, 2.5, 3); remembered |
| `return=<url>` | where "Exit game" goes when the tab cannot close itself (http/https only) |
| `lang=pl` / `deflang=pl` | the game language / the default one |

On your real site only these player options work (and `cursor`, `fps`,
`max`, `corpusMB`, `noprefetch`). The rest - e.g. `bridge=` or `corpus=` -
work only on a local page (`127.0.0.1`), so nobody can send your players a
link to your own site that talks to another server. A bridge on its own
address is set at build time: `[server] bridge` in `webclient.toml`.

The full list (also diagnostic switches): `m2w.options.table` in the browser
console, or [docs/REFERENCE.md](docs/REFERENCE.md).

---

## For developers

How the port works: the original C++ engine is compiled with Emscripten; a
compatibility layer replaces what the engine expected from Windows -
Direct3D 8 -> WebGL 2, Win32 -> browser APIs, Miles Sound -> Web Audio,
Granny 3D -> a clean-room `.gr2` reader, GDI text -> fonts baked with real
GDI, SpeedTree RT -> trees baked offline. Game data streams from the server
in 4 MB chunks (the "corpus") and is cached by the browser.

```
compat/          the compatibility layer (C++/JS) - see docs/REFERENCE.md
  tree/          our headers that replace a few engine headers
  tests/         unit tests
site/            the web page, Service Worker, download progress
bridge/          the WebSocket -> TCP bridge (Node.js)
tools/           build, packaging and baking tools; workspace.py reads webclient.toml
tools/gates/     quality gates (exports, shaders, model probe, login screen, tests, docs)
webclient.py     the one command that drives everything
docs/            BUILDING, DEPLOYMENT (EN) / WDROZENIE (PL), REFERENCE (generated)
```

- Every function has a `///` description (checked by
  `tools/gates/check_docs.py`); `docs/REFERENCE.md` lists every file,
  function and URL option.
- Where the engine sources had to change, `tools/stage_port.py` patches a
  copy while assembling the build tree; each patch says what it fixes and
  why.
- Comments say WHY: what was measured, what was tried and did not work.
- After a change: `python webclient.py check`. A difference is either a
  regression or an intended change (then `python webclient.py check --record`).

## Security

Found a vulnerability? Report it privately - see [SECURITY.md](SECURITY.md).

## License

GPL-2.0-or-later for everything in this repository (required by LZO, which
the client links). Copyright (C) 2026 nairoad - see [LICENSE](LICENSE) and
[NOTICE](NOTICE). The engine sources, game data and the SDKs used by the
offline bakers are **not** part of this repository and not covered by it;
their status is that of any Metin2 private server. The SpeedTree baker only
calls the SDK on your machine - nothing from it ends up in `client.wasm`.

## Credits

- Ymir Entertainment - the Metin2 (TMP4) engine.
- Built round by round with Claude (Anthropic).

If this project helped you - you run it on your server, built on it, or just
learned something from it - it would be nice if you mentioned where it came
from. A short "thanks to nairoad" with a link here is plenty. It is not a
condition of the license, just a way for me to see that the work found its
way to people. Thank you!
