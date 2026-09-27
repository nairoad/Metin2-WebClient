# compat/ - the browser layer

The TMP4 client is written for Windows (Win32, Direct3D 8, Granny, Miles,
DirectShow, CPython 2). This folder is what lets that code compile with
Emscripten **unchanged** and run in a browser: the Windows APIs it calls are
implemented here on top of WebGL 2, Web Audio, the DOM and WebSockets.

Every function has a `///` description; [docs/REFERENCE.md](../docs/REFERENCE.md)
lists every file and symbol.

## Where to start

- `main_web.cpp` - the entry point (`main` -> the client's `WinMain`).
- `runtime.js` - the JavaScript side: page options, the bridge address,
  fetching game data, the canvas. Linked into `client.js`.
- `win32_compat.h` / `win32_compat.cpp` - the core of the Win32 layer.

## By topic

| topic | files |
|---|---|
| Windows APIs | `win32_compat.*`, `platform_none.cpp` (what the browser does not have), `platform_crash.cpp`, `platform_codec.cpp` + `codepages.*`, `paths_web.cpp`, `locale_web.cpp`, `stubs.*` |
| Windows headers | `windows.h`, `winsock.h`, `mmsystem.h`, ... - short redirects so TMP4's `#include`s resolve to this layer |
| Input | `events_web.cpp`, `input_web.cpp`, `keyboard_web.cpp`, `cursor_web.cpp`, `ime_web.cpp` |
| Direct3D 8 on WebGL 2 | `d3d8*.h`, `d3d8_*.cpp` (constants, formats, fixed-function pipeline -> GLSL), `gl_*.cpp` + `gl_internal.h` (the device), `grpdevice_gl.cpp`, `grpdetector_gl.cpp` |
| Textures and images | `dxt.*`, `d3d8_image.*`, `jpeg_decode.*`, `d3dx8_textures.cpp`, `devil_web.cpp` |
| Characters | `custom_draw.*` (the shader road for skinned models) |
| Granny models and animation | `gr2_file.*` (the `.gr2` reader), `gr2_oodle1.*`, `gr2_to_granny.*`, `granny_web.cpp`, `granny_pose.cpp`, `granny_control.cpp` |
| Trees | `speedtree_web.cpp` (reads the bake of `tools/speedtree_bake`) |
| Game data | `webfs.*`, `webfs_web.cpp` (the streamed corpus), `properties_web.cpp` |
| Text | `platform_text.cpp` (GDI text on baked fonts and the canvas) |
| Sound and video | `sound_web.cpp` (Web Audio), `movie_web.cpp` (`<video>`) |
| Network | `network_web.cpp` (every TCP connection goes through the WebSocket bridge) |
| In-game browser | `webbrowser_web.cpp` |
| Python | `python2_bridge.*` (the Python 2 C API on CPython 3), `python_marshal_web.cpp`, `link_data.cpp` |
| Measuring | `frame_stats.*` |

## Folders

- `tree/` - TMP4 headers replaced as a whole (IME, sound, graphics detection).
- `Python-2.7/` - a redirect: the client embeds CPython 3, not 2.7.
- `tests/` - unit tests, run by `python tools/gates/run_tests.py`
  (part of `python webclient.py check`).
