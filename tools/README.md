# tools/ - building, packaging and checking

You normally do not run these one by one: `python webclient.py build` runs
the build steps in order (`python webclient.py steps` lists them), and
`python webclient.py check` runs the quality gates. Every tool also runs on
its own (`python tools/<name>.py`); its header says what it does.

## The build steps

| step | tool |
|---|---|
| stage | `stage_port.py` - copies the TMP4 sources into `build/port/stage` and applies our patches |
| lzo, cryptopp, eterbase, eterpack | `build_lzo.py`, `build_cryptopp.py`, `build_eterbase.py`, `build_eterpack.py` |
| python | `build_python.py` - the prebuilt CPython for wasm (or `--from-source`) |
| engine | `build_gamelib.py` - the client engine and the `compat/` layer |
| unpack, scripts | `build_corpus.py --unpack-only`, `unpack_packs.py` + `unpack_pack.cpp` (the pack reader) |
| rewrite, uiscript | `rewrite_scripts.py` + `script_patches.py` + `division.py` (Python 2 -> 3), `uiscript_from_client.py` |
| trees | `bake_speedtree.py` + `speedtree_bake/` (bakes SpeedTree trees to meshes with the SDK DLL) |
| gr2 | `repair_gr2.py` + `gr2_repair/` (re-saves the few `.gr2` files our reader refuses) |
| corpus | `build_corpus.py` - the streamed data (`manifest.bin` + 4 MB chunks) |
| data | `build_client_data.py` + `bake_fonts.py` + `cursors.py` - the start-up package |
| link | `link_client.py` - `client.wasm`, `client.js`, `client.data`, the page |

## Putting it on a server

- `package.py` - `dist/site/`, the files to upload (`python webclient.py package`).
- `bridge_config.py` - the bridge's allowlist and allowed pages
  (`--site https://your.site`).
- `site_server.py` - the local server behind `python webclient.py serve`.

## Readers and helpers

- `workspace.py` - where everything is (`webclient.toml`, defaults); run it to
  print the resolved settings.
- `webfs.py`, `check_corpus.py`, `chunk_order.py` - the streamed corpus.
- `gr2.py`, `oodle1.py`, `spt.py` - readers of the `.gr2` and `.spt` formats.
- `dxt_crosscheck.py`, `animation_review.py` - measuring aids.

## Folders

- `gates/` - the quality gates of `python webclient.py check`, their
  recorded baselines, and `reference.py`, which writes `docs/REFERENCE.md`.
- `browser/` - headless Edge that runs the client for the gates.
- `gr2_repair/`, `speedtree_bake/` - the native helpers (MSVC, x86).
- `data/` - `file_order.txt`, the recorded order the game reads its files in.
