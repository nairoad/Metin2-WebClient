# Building your WebClient

One command drives the whole build: `python webclient.py <command>`. It runs
the tools in `tools/` in the right order, stops at the first failure and says
in one sentence what to do about it. The full output of a build is also in
`build/webclient-build.log`.

## 1. What you bring

| input | `webclient.toml` key | notes |
|---|---|---|
| your client sources | `[inputs] source` | a directory with `source/<library>` (GameLib, EterLib, UserInterface, ...) and `extern/include`, `extern/library` |
| your installed game client | `[inputs] client` | the packs (`pack/*.eix`, `*.epk`), `SpeedTreeRT.dll`, `granny2.dll` |
| the UI font | `[inputs] font` | a `tahoma.ttf`; default: the Windows system font |
| your game server address | `[server] address` | private - never commit it |

Copy `webclient.example.toml` to `webclient.toml` (git-ignored) and set what
differs on your machine. `python tools/workspace.py` prints what the tools
will use.

## 2. Tools

| tool | why | required |
|---|---|---|
| Python 3.11+ (3.13 recommended) | runs everything; 3.13 also provides the standard library packed into the client | yes |
| Emscripten SDK 6.0.8 | compiles the engine to WebAssembly (`[tools] emsdk`); 6.0.8 matches the prebuilt CPython (section 5) | yes |
| Python 3.12 or older | still has `lib2to3`, which rewrites the game's Python 2 scripts (`[tools] python2to3`) | yes |
| Pillow (`pip install pillow`) | converts the mouse cursors | yes |
| Node.js 20+ (with `npm`) | runs the bridge (`cd bridge && npm ci` once) and the measurement harness | yes |
| CPython 3.13 built for wasm | the game's script engine inside `client.wasm` - the `python` step downloads it prebuilt (section 5) | yes |
| Git for Windows `sh` + GNU `make` + Python **3.13** | only to build CPython for wasm yourself (`build_python.py --from-source`) | no |
| LZO 2.10 source archive | the pack decompressor (the `lzo` step downloads it once and checks the SHA-256) | yes |
| Windows | bakes the UI fonts with real GDI (pixel-exact text) | optional |
| MSVC Build Tools 2022 (x86) | bakes the SpeedTree trees and re-saves broken `.gr2` models (`[tools] vcvars32`) | optional |

Run `python webclient.py doctor`: every missing item is one line with the
fix. Optional items only turn their step into a warning - the client builds
without them (no trees, text drawn by the browser, ~1% of models missing).

## 3. Build

```
python webclient.py doctor
python webclient.py build
python webclient.py serve
```

`python webclient.py steps` lists the steps. After fixing a failure, continue
where it stopped: `python webclient.py build --from <step>`. One step alone:
`--only <step>`; leave optional steps out: `--skip trees,gr2` (skipping a
required step leaves its output from an earlier build, or nothing - the
later steps then fail).

The client reaches your game server through a WebSocket-to-TCP bridge (a
browser cannot open TCP). `serve` starts ours (`bridge/`) next to the page;
it needs `cd bridge && npm ci` once, and connects only to the addresses of
your game's server list (plus `[bridge] targets`). On a real site: see
[DEPLOYMENT.md](DEPLOYMENT.md).

## 4. When a step fails

- **A patch does not match (`DID NOT HIT`)** - `tools/stage_port.py` patches
  the client sources, and your fork differs from ours at that place. The
  message names the patch and the file; each patch carries the reason it
  exists. Adapt the patch to your code (the same fix, your text).
- **A file does not compile** - the lines with `error:` name the file and the
  line. Usually a Windows API or a header the compatibility layer (`compat/`)
  does not provide yet.
- **Anything else** - the last lines of the step, and
  `build/webclient-build.log`.

## 5. CPython for wasm

The client embeds CPython 3.13 compiled for `wasm32-emscripten`:
`build/port/lib/libpython3.13.a`, `libmpdec.a`, `libHacl_Hash_SHA2.a` and the
headers of the SAME build in `build/port/python313/Include` (the headers of a
desktop Python do not fit: pointer size 8 instead of 4).

**By default the `python` step downloads them prebuilt** - one archive
(~18 MB) from this project's releases, checked against the SHA-256 written in
`tools/build_python.py` (a different file is refused) and unpacked into place.
Nothing else is needed. The archive holds the three libraries, the headers,
CPython's license texts and `BUILD-INFO.txt` (how it was built and the
SHA-256 of every file). It was made with **Emscripten 6.0.8**: with another
version the step warns, and if the link then fails, install 6.0.8 or build
it yourself.

No network on the build machine? Download the archive yourself (the URL is
`PREBUILT_URL` in `tools/build_python.py`) and run
`python tools/build_python.py --archive <file>`.

**Building it yourself:** `python tools/build_python.py --from-source`
downloads `Python-3.13.15.tar.xz` from python.org once (SHA-256 checked),
configures the standard wasm32-emscripten cross build (`--disable-ipv6`, five
unused modules left out) and installs the result - several minutes. It needs
Python 3.13 to run it (a cross build uses a native Python of the same
version), a POSIX `sh` (Git for Windows) and GNU `make`
(`winget install ezwinports.make`).

## 6. Checking your build

The probe and behaviour gates drive Microsoft Edge headless through
`tools/browser` - install its one library once: `cd tools/browser && npm ci`.

`python webclient.py check --record` records the quality-gate baselines from
a build you trust (the exported symbols, the generated shaders, a rendered
model, the login screen, the unit tests). After every change,
`python webclient.py check` compares against them: a difference is either a
regression or an intended change (then record again).

## 7. Publishing

`python webclient.py package` assembles `dist/site/` - the files to upload.
Hosting, HTTPS and the bridge: [DEPLOYMENT.md](DEPLOYMENT.md).
