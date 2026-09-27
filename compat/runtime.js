// SPDX-License-Identifier: GPL-2.0-or-later
// runtime.js - the JavaScript side of the compatibility layer, linked into
// client.js as `--pre-js`: options from the page address, screen and canvas
// measures, and what the page does when the client ends. C++ reaches it
// through one-line EM_JS bridges (platform_none.cpp, ...).
//
// Design (the renaming plan, section "runtime.js"):
// The build has no -sMODULARIZE, so this file is pasted into client.js at
// top level, right after `Module` is declared and BEFORE `HEAPU8`, `FS` or
// `Module.canvas` exist. Therefore the top level only DEFINES functions and
// the `m2w` object; nothing here touches the runtime until a function is
// called later, from C++. Functions called from here by the emitted EM_JS
// code (`m2w_ui_scale()`) keep working because the bridges stay in the
// glue under their C names.
//
// `window.m2w` is never overwritten: client.html may have put settings in
// it before client.js loaded (`m2w.config.bridge`), and every field is only
// added if missing.

var m2w = globalThis.m2w = globalThis.m2w || {};
m2w.config = m2w.config || {};

// ---------------------------------------------------------------------------
// Options: `?name=value` in the page address
// ---------------------------------------------------------------------------
// Every option has a `name` (the address parameter), the `file` that reads
// it and a `doc` line. `reference.py` reads this table into docs/REFERENCE.md.
//
// ON A REAL SITE ONLY THE PLAYER OPTIONS WORK (`site: true`: scale, language,
// return address, cursor, frame and memory limits). Everything else - the
// bridge and corpus addresses, probes, drawing switches - is read only on a
// local page (file://, localhost, 127.0.0.1): a link to the operator's real
// site with `?bridge=wss://attacker` or `?corpus=https://attacker/` would
// otherwise send the player's login, or load the attacker's game scripts,
// under the real address bar (security audit). On a site the
// bridge address comes from the page itself (m2w.config.bridge).

/// True on a page opened from this machine (file://, localhost, 127.0.0.1,
/// [::1]) - where the diagnostic options are allowed.
m2w.isLocalPage = function () {
    try {
        var loc = globalThis.location;
        return loc.protocol === 'file:' || /^(localhost|127\.0\.0\.1|\[::1\])$/.test(loc.hostname);
    } catch (e) {
        return false;
    }
};

m2w.options = m2w.options || {
    table: [
        { name: 'scale',  file: 'platform_none.cpp', site: true,
          doc: 'GUI scale 1-4, snapped to a baked font scale; remembered in localStorage' },
        { name: 'max',    file: 'platform_none.cpp', site: true,
          doc: 'drawing-buffer limit WxH (default 1920x1080); 0 = no limit' },
        { name: 'cursor', file: 'platform_none.cpp', site: true,
          doc: 'software | hardware; default from metin2.cfg' },
        { name: 'return', file: 'platform_none.cpp', site: true,
          doc: 'page to go back to when the client ends' },
        { name: 'run',    file: 'main_web.cpp',
          doc: '0 = link check only, do not enter WinMain' },
        { name: 'missing',  file: 'paths_web.cpp',
          doc: '1 = write every file nobody could find to /missing.txt' },
        { name: 'nomodels', file: 'paths_web.cpp',
          doc: '1 = every .gr2 is treated as missing (negative control)' }
    ],

    /// Raw value of an option by its name, or null when the address has none
    /// - and null for a non-player option on a page that is not local.
    get: function (name) {
        try {
            if (!m2w.isLocalPage()) {
                var row = null;
                for (var i = 0; i < this.table.length; ++i)
                    if (this.table[i].name === name) { row = this.table[i]; break; }
                if (!row || !row.site) return null;
            }
            return new URLSearchParams(globalThis.location.search).get(name);
        } catch (e) {
            return null;
        }
    },

    /// Adds rows from another file (each compat file registers its own).
    add: function (rows) {
        for (var i = 0; i < rows.length; ++i) this.table.push(rows[i]);
    }
};

// ---------------------------------------------------------------------------
// Screen measures
// ---------------------------------------------------------------------------

/// GUI scale: the game sees a LOGICAL screen =
/// physical pixels / scale (`?scale=2` -> windows and fonts 2x larger), while
/// the canvas and the drawing buffer stay PHYSICAL - the 3D world in full
/// resolution, interface quads multiplied in the shader (gl_device.cpp), fonts
/// baked at `lfHeight * scale` (platform_text.cpp), so letters are sharp,
/// not enlarged. Only scales with baked fonts are allowed
/// (tools/bake_fonts.py SKALE); the choice is remembered in
/// localStorage `m2w.scale`. Cached after the first call.
m2w.uiScale = function () {
    if (m2w.uiScaleValue) return m2w.uiScaleValue;
    var scale = 1;
    try {
        var v = m2w.options.get('scale');
        if (v === null) v = localStorage.getItem('m2w.scale');
        else localStorage.setItem('m2w.scale', v);
        var f = parseFloat(v);
        if (f >= 1 && f <= 4) scale = f;
    } catch (e) { }
    var allowed = [1, 1.25, 1.5, 1.75, 2, 2.5, 3];
    var nearest = allowed[0];
    for (var i = 1; i < allowed.length; ++i)
        if (Math.abs(allowed[i] - scale) < Math.abs(nearest - scale)) nearest = allowed[i];
    if (nearest !== scale) console.warn('m2w scale: ' + scale + ' -> ' + nearest + ' (baked fonts)');
    m2w.uiScaleValue = nearest;
    return nearest;
};

/// Effective device pixel ratio with the RESOLUTION CAP: the
/// drawing buffer does not exceed 1920x1080 (user: "Full HD at most"). On a
/// larger screen the game gets 1080p and the browser stretches the canvas
/// (CSS 100%) over the whole area - like the Windows client at 1920x1080 on
/// a 4K monitor. One factor, min(devicePixelRatio, 1920 / CSS width,
/// 1080 / CSS height), goes through EVERY screen measure (GetSystemMetrics,
/// desktop mode, canvas size, mouse). `?max=WxH` changes the limit,
/// `?max=0` removes it.
m2w.effectiveDpr = function () {
    var dpr = globalThis.devicePixelRatio || 1;
    try {
        if (m2w.maxBuffer === undefined) {
            var v = m2w.options.get('max');
            var w = 1920, h = 1080;
            if (v !== null) {
                var mm = /^(\d+)x(\d+)$/.exec(v);
                if (mm) { w = parseInt(mm[1], 10); h = parseInt(mm[2], 10); }
                else if (v === '0') { w = 0; h = 0; }
            }
            m2w.maxBuffer = { w: w, h: h };
            if (w > 0) console.log('m2w screen: buffer limit ' + w + 'x' + h + ' (?max=)');
        }
        var k = m2w.maxBuffer;
        var cw = globalThis.innerWidth || 0, ch = globalThis.innerHeight || 0;
        if (k.w > 0 && cw > 0 && ch > 0) {
            var f = Math.min(dpr, k.w / cw, k.h / ch);
            if (f < dpr) dpr = f;
        }
    } catch (e) { }
    return dpr;
};

/// LOGICAL pixels per CSS pixel: effective dpr / GUI scale. Every "screen
/// size" the game asks for goes through this.
m2w.cssToBufferRatio = function () {
    return m2w.effectiveDpr() / m2w.uiScale();
};

/// Cursor mode from the address: 1 = software (the game draws it, browser
/// cursor hidden - cursor_web.cpp), 2 = hardware (CSS), 0 = not given
/// (metin2.cfg decides).
m2w.cursorMode = function () {
    var v = m2w.options.get('cursor');
    if (v === 'software') return 1;
    if (v === 'hardware') return 2;
    return 0;
};

/// Canvas state for WM_SIZE: what = 0 width, 1 height (both in LOGICAL
/// pixels), 2 = the tab is hidden (1/0).
///
/// The DISPLAYED size, not the buffer: `canvas.width/height` is
/// the buffer we set at Reset, so after a window resize it looked unchanged,
/// WM_SIZE never went out and a 1280x150 image was stretched over 1253x918.
/// PHYSICAL pixels: CSS pixels at 150 % Windows scaling gave a
/// 1677x1278 buffer stretched to 2516x1917 - every glyph blurred 1.5x.
/// `GetSystemMetrics` multiplies by the same ratio, so both measures agree.
m2w.canvasState = function (what) {
    try {
        if (what == 2)
            return (typeof document !== 'undefined' && document.hidden) ? 1 : 0;
        var canvas = (typeof Module !== 'undefined' && Module.canvas)
                   ? Module.canvas
                   : document.getElementById('canvas');
        var ratio = m2w.cssToBufferRatio();
        if (canvas) {
            var n = (what == 1) ? canvas.clientHeight : canvas.clientWidth;
            if (n > 0) return Math.round(n * ratio);
            n = (what == 1) ? canvas.height : canvas.width;
            if (n > 0) return n;
        }
        return Math.round(((what == 1) ? (globalThis.innerHeight || 0)
                                       : (globalThis.innerWidth || 0)) * ratio);
    } catch (e) {
        return 0;
    }
};

/// Window (= screen) size in LOGICAL pixels for GetSystemMetrics.
/// `innerWidth/innerHeight` are CSS pixels; without the ratio the game got a
/// smaller "screen" than the monitor, drew a smaller buffer and the browser
/// stretched it - blurred text at 150 % Windows scaling.
m2w.windowSize = function (isHeight) {
    try {
        var css = isHeight ? (globalThis.innerHeight || 0) : (globalThis.innerWidth || 0);
        return Math.round(css * m2w.cssToBufferRatio());
    } catch (e) {
        return 0;
    }
};

// ---------------------------------------------------------------------------
// End of the game
// ---------------------------------------------------------------------------

/// `u` as an absolute address when it is an http(s) page (relative addresses
/// are resolved against this page), '' otherwise. Everything that navigates
/// to an address from outside - `?return=`, the in-game browser - goes
/// through here: a `javascript:` or `data:` address would run code in the
/// player's page (a link to the real site with `?return=javascript:...`).
m2w.safeUrl = function (u) {
    try {
        var a = new URL(String(u), globalThis.location.href);
        return (a.protocol === 'https:' || a.protocol === 'http:') ? a.href : '';
    } catch (e) {
        return '';
    }
};

/// What the page does after PostQuitMessage (user decision): first
/// try to close the tab, and when the browser refuses (only a tab opened by
/// script may close itself) go back where the player came from - `?return=`,
/// `m2w.config.returnUrl` set by the page, or `document.referrer` on the
/// same origin - and with none of these, show a message. The client carries
/// NO page address of its own; that is a deployment setting.
m2w.quit = function () {
    try {
        var target = m2w.options.get('return') || '';
        if (!target && m2w.config.returnUrl) target = String(m2w.config.returnUrl);
        if (!target && document.referrer) {
            try {
                var r = new URL(document.referrer);
                if (r.origin === location.origin && r.href !== location.href) target = r.href;
            } catch (e) {}
        }
        target = target ? m2w.safeUrl(target) : '';
        var status = document.getElementById('status');
        /// Puts `t` in the page status line (`Module.setStatus`) and makes it visible.
        var show = function (t) {
            if (typeof Module !== 'undefined' && Module.setStatus) Module.setStatus(t);
            if (status) { status.className = ''; status.style.display = 'block'; }
        };
        show('The client has closed.');
        try { window.close(); } catch (e) {}
        setTimeout(function () {
            if (target) {
                show('The client has closed - returning to the site...');
                location.href = target;
            } else {
                show('The client has closed. Reload the page to play again (reason: syserr / console).');
            }
        }, 400);
    } catch (e) {}
};

// ---------------------------------------------------------------------------
// Heap helpers for the EM_JS bridges
// ---------------------------------------------------------------------------
// The C++ side passes pointers into the wasm heap; these write results
// there, so every bridge stays one line. `HEAP32` / `HEAPU8` /
// `HEAPF64` / `_malloc` are the module's own - resolved at call time, so
// they are the current views even after memory growth.

/// Copies `bytes` (a Uint8Array, or null) into a new `malloc` buffer and
/// writes its length under `piLength`; returns the pointer, or 0 (length 0)
/// for null. The caller frees it.
m2w.heapBytes = function (bytes, piLength) {
    if (!bytes) { HEAP32[piLength >> 2] = 0; return 0; }
    var p = _malloc(bytes.length ? bytes.length : 1);
    HEAPU8.set(bytes, p);
    HEAP32[piLength >> 2] = bytes.length;
    return p;
};

/// `s` as a NUL-terminated UTF-8 string in a new `malloc` buffer (the
/// caller frees it).
m2w.heapString = function (s) {
    var n = lengthBytesUTF8(s) + 1;
    var p = _malloc(n);
    stringToUTF8(s, p, n);
    return p;
};

/// Writes the numbers of `values` as 32-bit ints from address `p` on.
m2w.heapInts = function (p, values) {
    for (var k = 0; k < values.length; ++k) HEAP32[(p >> 2) + k] = values[k];
};

/// Writes the numbers of `values` as doubles from address `p` on.
m2w.heapDoubles = function (p, values) {
    for (var k = 0; k < values.length; ++k) HEAPF64[(p >> 3) + k] = values[k];
};

// ---------------------------------------------------------------------------
// Start (main_web.cpp)
// ---------------------------------------------------------------------------

/// 0 when the address says `?run=0` (do not start the client), else 1 -
/// also 1 when the page replaced `m2w.options` with its own object.
m2w.shouldRun = function () {
    try {
        return m2w.options.get('run') === '0' ? 0 : 1;
    } catch (e) {
        return 1;    // e.g. the page replaced `m2w.options` with its own object
    }
};

// ---------------------------------------------------------------------------
// Mouse and characters as window messages (events_web.cpp)
// ---------------------------------------------------------------------------
// Listeners put events into a queue. The queue is drained once per frame
// by `PeekMessage` (platform_none.cpp) - the same "poll, do not listen"
// decision as WM_SIZE - with ONE exception: a right or middle press is
// handed to the game at once (`m2w.pumpNow`, see the mousedown listener).
// `m2w.events` is the state: `queue`, virtual position `x, y` (what the
// game sees; `fx, fy` the same with the fraction kept), the last real
// position `rx, ry`, `buttons` (bit mask of held buttons), `drag` (the
// game turns the camera), `lockRequested` / `lockAskedAt` (pointer lock
// asked for, not yet granted), `lockBroken` (no lock for this drag - the
// fallback path), and the diagnostic counters `rejected` (movement jumps
// discarded) and `lockFailed`.
m2w.events = m2w.events || null;

/// Installs the listeners on the canvas and the window. Safe to repeat;
/// does nothing until the canvas exists.
m2w.eventsStart = function () {
    if (m2w.events || !globalThis.document) return;

    var canvas = (typeof Module !== 'undefined' && Module.canvas)
               ? Module.canvas
               : document.getElementById('canvas');
    if (!canvas) return;

    var state = { queue: [], x: 0, y: 0, fx: 0, fy: 0, rx: 0, ry: 0,
                  buttons: 0, drag: false, lockRequested: false,
                  lockAskedAt: 0, lockBroken: false, rejected: 0,
                  lockFailed: 0 };
    m2w.events = state;

    // Client coordinates = canvas pixels from its top-left corner.
    // `getBoundingClientRect` gives the canvas position on the page, and
    // `width / rect.width` converts CSS pixels to buffer pixels (not the
    // same when the canvas is scaled by style). The buffer is PHYSICAL and
    // the game counts in LOGICAL pixels (physical / GUI scale),
    // hence the division by the scale.
    /// CSS pixels of canvas rectangle `r` -> LOGICAL game pixels (buffer / GUI
    /// scale), as {sx, sy}; 1 for an empty rectangle.
    var scaleFactors = function (r) {
        var scale = m2w.uiScale();
        return { sx: r.width  ? (canvas.width  / scale / r.width)  : 1,
                 sy: r.height ? (canvas.height / scale / r.height) : 1 };
    };
    /// Stores the event's position in logical game pixels as both the real
    /// (`rx`, `ry`) and the virtual (`x`, `y`) cursor.
    var position = function (e) {
        var r = canvas.getBoundingClientRect();
        var f = scaleFactors(r);
        state.rx = Math.round((e.clientX - r.left) * f.sx);
        state.ry = Math.round((e.clientY - r.top)  * f.sy);
        state.x = state.fx = state.rx;
        state.y = state.fy = state.ry;
    };

    /// Queues a window message with the current virtual cursor; above 256
    /// waiting messages the oldest is dropped.
    var push = function (message, wParam) {
        // The queue grows only between frames; if the client stalls there
        // is no reason to grow without bound - old input is useless anyway.
        if (state.queue.length > 256) state.queue.shift();
        state.queue.push({ k: message, w: wParam, x: state.x, y: state.y });
    };

    var buttonDown = [0x0201, 0x0207, 0x0204];  // WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN
    var buttonUp   = [0x0202, 0x0208, 0x0205];

    // CAMERA TURN = POINTER LOCK. The original computes the turn
    // speed from the DISTANCE to the anchor point while dragging with the
    // right (or middle) button and moves the cursor back to it with
    // `SetCursorPos` every move (PythonApplicationEvent.cpp,
    // `CCamera::Drag`). A browser cannot move the cursor, so the distance
    // grew with every millimetre and the camera "sped up and spun without
    // end" (user). Pointer Lock gives RELATIVE movement (`movementX/Y`);
    // the virtual position is what `SetCursorPos` moves back.
    //
    // THE LOCK FOLLOWS THE GAME, NOT THE BUTTON. It used to be requested on
    // every right press, but a right press is not always a camera turn: on
    // an inventory slot it drinks a potion, and the page still went into
    // "turn" mode (user). The game itself says when it turns the camera -
    // `CCamera::BeginDrag` / `EndDrag`, patched to call `m2w.cameraDrag`
    // (tools/stage_port.py) - and only then is the lock requested.
    /// True while the canvas holds the pointer lock.
    var locked = function () {
        return document.pointerLockElement === canvas;
    };
    /// True while movement is RELATIVE: the game turns the camera, or a
    /// lock is held, or one was requested in the last second (it may still
    /// arrive after a quick click - see the mousemove listener).
    var relative = function () {
        return state.drag || locked() ||
               (state.lockRequested && performance.now() - state.lockAskedAt < 1000);
    };
    /// No lock for this drag: the fallback path, the cursor stays visible.
    var lockFailed = function () {
        state.lockFailed += 1;
        state.lockRequested = false;
        state.lockBroken = true;
        m2w.cursorRefresh();
    };

    /// The game started (1) or ended (0) a camera drag.
    m2w.cameraDrag = function (on) {
        if (on) {
            if (state.drag) return;
            state.drag = true;
            state.lockBroken = false;
            // Relative from HERE: the virtual position starts at the real
            // one, fraction cleared.
            state.fx = state.x;
            state.fy = state.y;
            if (locked()) {
                // nothing to ask for
            } else if (!canvas.requestPointerLock) {
                state.lockBroken = true;
            } else {
                // `requestPointerLock` is ASYNCHRONOUS; from the request on
                // the cursor counts as hidden - Firefox warps it to the
                // centre when the lock arrives, and a visible cursor showed
                // that as "for a split second the mouse goes to the middle"
                // (user). Firefox grants a lock only while a user input
                // handler runs, which is why a right press is handed to the
                // game at once (mousedown below).
                state.lockRequested = true;
                state.lockAskedAt = performance.now();
                try {
                    var p = canvas.requestPointerLock();
                    // A refusal also fires `pointerlockerror`, which counts it.
                    if (p && p.catch) p.catch(function () { });
                } catch (err) {
                    lockFailed();
                }
            }
        } else {
            state.drag = false;
            // After the lock ends the real cursor is where it was on press;
            // the virtual position returns to it with the first ordinary
            // `mousemove`. A lock still on its way is let go when it comes
            // (pointerlockchange).
            if (locked() && document.exitPointerLock) document.exitPointerLock();
        }
        m2w.cursorRefresh();
    };

    // Movement is caught on the WINDOW: dragging with a button held takes
    // the cursor outside the canvas, and the turn (or an item being
    // dragged) must go on - Windows kept delivering it to the captured
    // window.
    globalThis.addEventListener('mousemove', function (e) {
        if (e.target !== canvas && !state.buttons && !relative()) return;
        // While RELATIVE, clientX/Y is never taken: Firefox, enabling the
        // lock, warps the cursor to the window centre and sends a
        // `mousemove` with the centre as clientX/Y BEFORE
        // `pointerLockElement` is set - also when the button is already up
        // (a quick click); taken as a hand movement it put the game cursor
        // in the middle "for a second" (user). `movementX/Y` exists in
        // every browser without a lock too.
        if (relative()) {
            // Firefox (Windows) also warps the hidden cursor to the centre
            // DURING the lock and reports it as movement of hundreds of
            // pixels; summed like a hand movement it gave "the camera jumps
            // now and then" (user). A hand moves tens of pixels between
            // events; the threshold of a quarter of the window (min 200 CSS
            // px) cuts only the warps. Rejected ones are counted.
            var r = canvas.getBoundingClientRect();
            var threshold = Math.max(200, r.width / 4);
            if (Math.abs(e.movementX) > threshold || Math.abs(e.movementY) > threshold) {
                state.rejected += 1;
                return;
            }
            // THE FRACTION IS KEPT. One CSS pixel is less than one game
            // pixel when the GUI scale is above the device pixel ratio
            // (0.5 at scale 2, or at scale 1 on a 4K screen); rounded every
            // event, slow movement left or up vanished
            // (Math.round(n - 0.5) = n) while right or down did not.
            var f = scaleFactors(r);
            state.fx += e.movementX * f.sx;
            state.fy += e.movementY * f.sy;
            state.x = Math.round(state.fx);
            state.y = Math.round(state.fy);
        } else {
            position(e);
        }
    });
    document.addEventListener('pointerlockerror', lockFailed);
    document.addEventListener('pointerlockchange', function () {
        state.lockRequested = false;
        if (locked()) {
            // The lock came after the drag ended (press and release before
            // the browser granted it). Nothing turns - let it go, or the
            // cursor stays captured.
            if (!state.drag && document.exitPointerLock) document.exitPointerLock();
        } else if (state.drag) {
            // Lost during a turn (Esc, focus loss): the fallback path.
            state.lockBroken = true;
        }
        m2w.cursorRefresh();
    });

    canvas.addEventListener('mousedown', function (e) {
        if (!relative()) position(e);
        if (e.button >= 0 && e.button <= 2) {
            state.buttons |= 1 << e.button;
            push(buttonDown[e.button], 0);
        }
        // A RIGHT OR MIDDLE PRESS GOES TO THE GAME AT ONCE, not with the
        // next frame: the game decides while handling it whether this is a
        // camera turn (`m2w.cameraDrag`), and Firefox grants a pointer lock
        // only while a user input handler runs. The whole queue is taken,
        // in order, so nothing overtakes anything.
        if (e.button === 1 || e.button === 2) m2w.pumpNow();
        // Focus on the canvas - without it the keyboard goes to the page.
        if (canvas.focus) canvas.focus();
        e.preventDefault();
    });

    // RELEASE IS CAUGHT ON THE WINDOW, NOT ON THE CANVAS: a button pressed
    // on the canvas and released outside it would otherwise leave the
    // client with the button down and the capture held forever. The lock
    // is let go by the game ending its drag (`m2w.cameraDrag(0)`).
    globalThis.addEventListener('mouseup', function (e) {
        if (e.button >= 0 && e.button <= 2) state.buttons &= ~(1 << e.button);
        if (!relative()) position(e);
        if (e.button >= 0 && e.button <= 2) push(buttonUp[e.button], 0);
        if (e.button === 1 || e.button === 2) {
            // The release goes to the game at once too, so the drag ends
            // (and the lock goes) in the same moment. Should the game keep
            // dragging with neither button held (a right release with an
            // item on the cursor skips `EndDrag` in game.py), the page does
            // not keep the pointer captured for it.
            m2w.pumpNow();
            if (state.drag && !(state.buttons & 6)) {
                state.drag = false;
                if (locked() && document.exitPointerLock) document.exitPointerLock();
                m2w.cursorRefresh();
            }
        }
    });

    // Without these the right button opens the browser menu instead of
    // turning the camera, and the wheel scrolls the page instead of zooming.
    canvas.addEventListener('contextmenu', function (e) { e.preventDefault(); });

    canvas.addEventListener('wheel', function (e) {
        position(e);
        // Windows carries a multiple of 120 in the HIGH word of `wParam`
        // and `PythonApplicationProcedure` reads `short(HIWORD(wParam))`.
        // The sign is opposite: in the browser `deltaY` grows "away".
        var steps = (e.deltaY > 0) ? -120 : 120;
        push(0x020A, (steps & 0xFFFF) << 16);              // WM_MOUSEWHEEL
        e.preventDefault();
    }, { passive: false });

    // CHARACTERS - the path of login and chat: a text field waits for
    // `WM_CHAR`, not for a key state. `e.key` of length one is a character;
    // anything longer is a key name ("Enter", "F5").
    globalThis.addEventListener('keydown', function (e) {
        var vk = 0;
        if (e.key === 'Backspace') vk = 0x08;
        else if (e.key === 'Tab') vk = 0x09;
        else if (e.key === 'Enter') vk = 0x0D;
        else if (e.key === 'Escape') vk = 0x1B;
        else if (e.key === 'Delete') vk = 0x2E;
        else if (e.key === 'ArrowLeft') vk = 0x25;
        else if (e.key === 'ArrowUp') vk = 0x26;
        else if (e.key === 'ArrowRight') vk = 0x27;
        else if (e.key === 'ArrowDown') vk = 0x28;
        else if (e.key === 'Home') vk = 0x24;
        else if (e.key === 'End') vk = 0x23;
        else if (e.key.length === 1) vk = e.key.toUpperCase().charCodeAt(0);
        if (vk) push(0x0100, vk);                            // WM_KEYDOWN

        // `WM_CHAR` carries the CHARACTER, case preserved.
        if (e.key.length === 1) push(0x0102, e.key.charCodeAt(0));
        else if (e.key === 'Enter') push(0x0102, 13);
        else if (e.key === 'Backspace') push(0x0102, 8);

        // Keys the browser uses for itself and the game for something
        // else. The rest stays with the page - F5, F12, Ctrl+... must work,
        // or we take our own tools away.
        if (e.key === 'Tab' || e.key.indexOf('Arrow') === 0) e.preventDefault();
    });

    // Focus loss - see the same note in input_web.cpp. Events from before
    // the loss no longer describe anything. A button held at that moment
    // will be released outside the page, where no `mouseup` reaches us:
    // it is released NOW, so the game ends its camera drag (and the page
    // its lock) instead of turning the camera with no button held.
    globalThis.addEventListener('blur', function () {
        state.queue.length = 0;
        for (var b = 0; b <= 2; ++b)
            if (state.buttons & (1 << b)) push(buttonUp[b], 0);
        state.buttons = 0;
    });
};

/// Next queued event `{k, w, x, y}` or null.
m2w.eventPoll = function () {
    var state = m2w.events;
    if (!state || !state.queue.length) return null;
    return state.queue.shift();
};

/// Takes one event off the queue into four ints at `pData` (message,
/// `wParam`, x, y); 0 when the queue is empty, else 1.
m2w.eventPollInto = function (pData) {
    var z = m2w.eventPoll();
    if (!z) return 0;
    m2w.heapInts(pData, [z.k, z.w, z.x, z.y]);
    return 1;
};

/// Moves the virtual cursor to (x, y) - what `SetCursorPos` does; 0 before
/// the events are installed.
m2w.mouseSet = function (x, y) {
    var state = m2w.events;
    if (!state) return 0;
    state.x = state.fx = x;
    state.y = state.fy = y;
    return 1;
};

/// Hands the queued messages to the game now (`M2W_PumpMessagesNow`, the
/// `CPythonApplication::Loop` patch in tools/stage_port.py); returns how
/// many were taken - 0 before the main loop runs or the module is ready.
m2w.pumpNow = function () {
    if (typeof runtimeInitialized === 'undefined' || !runtimeInitialized) return 0;
    if (typeof Module === 'undefined' || !Module._M2W_PumpMessagesNow) return 0;
    return Module._M2W_PumpMessagesNow();
};

// ---------------------------------------------------------------------------
// Keyboard state (input_web.cpp)
// ---------------------------------------------------------------------------

/// Installs the key-state table (`globalThis.m2w_keys`, 256 bytes indexed
/// by VK code) and the `keydown` / `keyup` / `blur` listeners that fill it.
/// Kept in JavaScript because that is where the events arrive; a query from
/// C++ is one boundary crossing per key per frame, and there are few pollers.
m2w.keysStart = function () {
    if (globalThis.m2w_keys || !globalThis.document) return;

    var state = new Uint8Array(256);
    globalThis.m2w_keys = state;

    // `KeyboardEvent.code` -> Windows virtual key. Only the keys the client
    // POLLS through `GetAsyncKeyState` are listed - the rest of the `VK_*`
    // family belongs to key bindings, which take another road.
    var map = {
        ShiftLeft: 0xA0, ShiftRight: 0xA1,      // VK_LSHIFT / VK_RSHIFT
        ControlLeft: 0xA2, ControlRight: 0xA3,  // VK_LCONTROL / VK_RCONTROL
        AltLeft: 0xA4, AltRight: 0xA5,          // VK_LMENU / VK_RMENU
        CapsLock: 0x14,                         // VK_CAPITAL
        Escape: 0x1B, Space: 0x20, Enter: 0x0D, Tab: 0x09,
        ArrowLeft: 0x25, ArrowUp: 0x26, ArrowRight: 0x27, ArrowDown: 0x28
    };
    // Letters and digits: the virtual key equals the ASCII code of the
    // upper-case character.
    for (var i = 0; i < 26; ++i)
        map['Key' + String.fromCharCode(65 + i)] = 65 + i;
    for (var c = 0; c < 10; ++c)
        map['Digit' + c] = 48 + c;
    for (var f = 1; f <= 12; ++f)
        map['F' + f] = 0x70 + (f - 1);          // VK_F1 = 0x70

    /// Records key event `e` as down (1) / up (0), with the either-side VK too.
    var set = function (e, value) {
        var vk = map[e.code];
        if (vk !== undefined) state[vk] = value;
        // `Shift`, `Control`, `Alt` also exist as "either of the pair" -
        // VK_SHIFT 0x10, VK_CONTROL 0x11, VK_MENU 0x12. TMP4 asks for both.
        if (e.code === 'ShiftLeft' || e.code === 'ShiftRight') state[0x10] = value;
        if (e.code === 'ControlLeft' || e.code === 'ControlRight') state[0x11] = value;
        if (e.code === 'AltLeft' || e.code === 'AltRight') state[0x12] = value;
    };

    globalThis.addEventListener('keydown', function (e) { set(e, 1); });
    globalThis.addEventListener('keyup', function (e) { set(e, 0); });

    // Losing focus is the one moment a key can stay "down forever": its
    // release outside the page never arrives. Win32 had the same problem
    // and the same answer - the state is cleared on WM_KILLFOCUS.
    globalThis.addEventListener('blur', function () { state.fill(0); });
};

/// Whether the key with this VK code is down now (0/1).
m2w.keyPressed = function (iVk) {
    var state = globalThis.m2w_keys;
    if (!state || iVk < 0 || iVk > 255) return 0;
    return state[iVk] ? 1 : 0;
};

// ---------------------------------------------------------------------------
// Intro movie (movie_web.cpp)
// ---------------------------------------------------------------------------

/// Puts a `<video>` overlay over the page and plays `bytes` (a copy taken
/// by the bridge - a `Blob` cannot hold a view of the wasm heap, which
/// moves whenever memory grows); `name` only for the warning.
m2w.moviePlay = function (bytes, name) {
    if (!globalThis.document) return;

    var old = document.getElementById('m2w_movie');
    if (old && old.parentNode) old.parentNode.removeChild(old);

    var blob = new Blob([bytes]);
    var url = URL.createObjectURL(blob);

    var v = document.createElement('video');
    v.id = 'm2w_movie';
    v.src = url;
    v.autoplay = true;
    v.style.position = 'absolute';
    v.style.left = '0';
    v.style.top = '0';
    v.style.width = '100%';
    v.style.height = '100%';
    v.style.objectFit = 'contain';
    v.style.background = '#000';

    /// Frees the object URL and removes the video element.
    var cleanUp = function () {
        URL.revokeObjectURL(url);
        if (v.parentNode) v.parentNode.removeChild(v);
    };
    v.onended = cleanUp;
    /// The browser cannot play the file: warn and remove the overlay.
    v.onerror = function () {
        console.warn('m2w movie: cannot play ' + name);
        cleanUp();
    };
    // Skipping - in TMP4 any key or a click did it.
    v.onclick = cleanUp;

    document.body.appendChild(v);

    // The browser may refuse to play with sound before the player's first
    // gesture; then try again MUTED - a silent movie beats none.
    var p = v.play();
    if (p && p.catch) {
        p.catch(function () { v.muted = true; v.play().catch(cleanUp); });
    }
};

// ---------------------------------------------------------------------------
// Cursor (cursor_web.cpp)
// ---------------------------------------------------------------------------
// `m2w.cursor`: `css` = the last shape the game set, `hidden` = what
// `ShowCursor` says, `software` = the game draws its own cursor and the
// browser's stays `none` (?cursor=software / metin2.cfg SOFTWARE_CURSOR).
m2w.cursor = m2w.cursor || { css: '', hidden: false, software: false };

/// The canvas element, if the page has one.
m2w.cursorCanvas = function () {
    return document.getElementById('canvas') || document.querySelector('canvas');
};

/// Writes `canvas.style.cursor` from the current state. CAMERA-TURN
/// FALLBACK: when the browser refuses the pointer lock (e.g. an
/// app panel: "root document not valid for pointer lock") the real cursor
/// follows the hand; hiding it then would make it reappear wherever it got
/// to on release ("the cursor jumps", user). So during a camera drag
/// with NO lock (`lockBroken`) it stays visible (in the "turn camera" shape the game just
/// set). A lock REQUESTED but not yet granted counts as a lock,
/// or the cursor is visible at the moment the browser warps it to the
/// centre.
m2w.cursorRefresh = function () {
    try {
        var canvas = m2w.cursorCanvas();
        if (!canvas) return;
        var ev = m2w.events;
        var noLock = ev && ev.drag && ev.lockBroken;
        canvas.style.cursor = m2w.cursor.software ? 'none'
            : ((!m2w.cursor.hidden || noLock) ? (m2w.cursor.css || '') : 'none');
    } catch (e) { }
};

/// The game set a shape: remember it and apply it when the cursor is shown.
/// The last value is remembered so that `cursorVisible(1)` returns to it
/// (earlier it set '' and lost the shape).
m2w.cursorSet = function (css) {
    try {
        m2w.cursor.css = css;
        var canvas = m2w.cursorCanvas();
        if (canvas && !m2w.cursor.hidden && !m2w.cursor.software)
            canvas.style.cursor = css;
    } catch (e) { }    // no canvas yet is not an error
};

/// `ShowCursor`: visible (1) or hidden (0).
m2w.cursorVisible = function (visible) {
    m2w.cursor.hidden = !visible;
    m2w.cursorRefresh();
};

// ---------------------------------------------------------------------------
// DirectInput scan codes (keyboard_web.cpp)
// ---------------------------------------------------------------------------
// `m2w.dik` is a 256-byte table of DirectInput scan codes (`DIK_*`), 1 while
// the key is down. `KeyboardEvent.code` is the key's POSITION regardless of
// layout (`KeyA` = the key at A on a QWERTY board), which is exactly what a
// scan code is, so the mapping is literal. Keys the browser keeps for itself
// never arrive (PrintScreen, F11, F12).
m2w.dik = m2w.dik || null;

/// Installs the listeners and the table. Safe to repeat.
m2w.dikStart = function () {
    if (m2w.dik || !globalThis.document) return;

    var map = {
        Escape:1, Digit1:2, Digit2:3, Digit3:4, Digit4:5, Digit5:6,
        Digit6:7, Digit7:8, Digit8:9, Digit9:10, Digit0:11,
        Minus:12, Equal:13, Backspace:14, Tab:15,
        KeyQ:16, KeyW:17, KeyE:18, KeyR:19, KeyT:20, KeyY:21, KeyU:22,
        KeyI:23, KeyO:24, KeyP:25, BracketLeft:26, BracketRight:27,
        Enter:28, ControlLeft:29,
        KeyA:30, KeyS:31, KeyD:32, KeyF:33, KeyG:34, KeyH:35, KeyJ:36,
        KeyK:37, KeyL:38, Semicolon:39, Quote:40, Backquote:41,
        ShiftLeft:42, Backslash:43,
        KeyZ:44, KeyX:45, KeyC:46, KeyV:47, KeyB:48, KeyN:49, KeyM:50,
        Comma:51, Period:52, Slash:53, ShiftRight:54,
        NumpadMultiply:55, AltLeft:56, Space:57, CapsLock:58,
        F1:59, F2:60, F3:61, F4:62, F5:63, F6:64, F7:65, F8:66, F9:67,
        F10:68, NumLock:69, ScrollLock:70,
        Numpad7:71, Numpad8:72, Numpad9:73, NumpadSubtract:74,
        Numpad4:75, Numpad5:76, Numpad6:77, NumpadAdd:78,
        Numpad1:79, Numpad2:80, Numpad3:81, Numpad0:82, NumpadDecimal:83,
        F11:87, F12:88,
        NumpadEnter:156, ControlRight:157, NumpadDivide:181, AltRight:184,
        Home:199, ArrowUp:200, PageUp:201, ArrowLeft:203, ArrowRight:205,
        End:207, ArrowDown:208, PageDown:209, Insert:210, Delete:211,
        MetaLeft:219, MetaRight:220, ContextMenu:221
    };

    var state = new Uint8Array(256);
    m2w.dik = state;

    document.addEventListener('keydown', function (e) {
        var d = map[e.code];
        if (d !== undefined) {
            state[d] = 1;
            // Game keys must not scroll or leave the page. F5 and F12 are
            // not blocked - they belong to the user, not to the game.
            if (e.code !== 'F5' && e.code !== 'F12')
                e.preventDefault();
        }
    }, true);

    document.addEventListener('keyup', function (e) {
        var d = map[e.code];
        if (d !== undefined) state[d] = 0;
    }, true);

    // Focus loss releases EVERYTHING. Otherwise a key held while switching
    // tabs stays down forever - the character runs by itself and nobody
    // knows why.
    globalThis.addEventListener('blur', function () {
        state.fill(0);
    });
};

// ---------------------------------------------------------------------------
// Language and font (locale_web.cpp)
// ---------------------------------------------------------------------------
m2w.options.add([
    { name: 'lang',    file: 'locale_web.cpp', site: true,
      doc: 'language of the game texts (en, de, pl, ...); remembered in localStorage m2/lang' },
    { name: 'deflang', file: 'locale_web.cpp', site: true,
      doc: 'default language given by the embedding page (the server language)' }
]);

/// Language -> Windows code page, with the three locale folder names that
/// differ from BCP 47 (cs -> cz, da -> dk, el -> gr).
m2w.locales = {
    en: 1252, de: 1252, fr: 1252, es: 1252, it: 1252, nl: 1252, pt: 1252, dk: 1252,
    cz: 1250, hu: 1250, pl: 1250, ro: 1250,
    ru: 1251, gr: 1253, tr: 1254
};

/// The language to play in, in this order:
/// `?lang=` (remembered), then localStorage `m2/lang`, then `?deflang=`,
/// then the browser languages, finally `en`. Returns {lang, codePage}.
m2w.browserLanguage = function () {
    var FROM_BROWSER = { cs: 'cz', da: 'dk', el: 'gr' };
    /// A supported language code from `s` (`pl-PL` -> `pl`; cs/da/el mapped to
    /// the client's cz/dk/gr), or '' when the client has no such locale.
    var pick = function (s) {
        s = String(s || '').toLowerCase().split('-')[0];
        if (FROM_BROWSER[s]) s = FROM_BROWSER[s];
        return m2w.locales[s] ? s : '';
    };
    var v = pick(m2w.options.get('lang'));
    if (v) { try { localStorage.setItem('m2/lang', v); } catch (e) {} }
    if (!v) { try { v = pick(localStorage.getItem('m2/lang')); } catch (e) {} }
    if (!v) v = pick(m2w.options.get('deflang'));
    if (!v) {
        try {
            var langs = navigator.languages || [navigator.language];
            for (var i = 0; i < langs.length && !v; i++) v = pick(langs[i]);
        } catch (e) {}
    }
    if (!v) v = 'en';
    return { lang: v, codePage: m2w.locales[v] };
};

/// Registers `/tahoma.ttf` from the preloaded package (MEMFS, before main)
/// as the browser font "Tahoma". MEASURED, not guessed: the game asks for
/// UI_DEF_FONT = "Tahoma:12", `CreateFontIndirectA` (platform_text.cpp)
/// turns it into CSS `font-family: "Tahoma"` and LEAVES THE CHOICE TO THE
/// BROWSER - without our own file the result depends on what the system
/// substitutes (user: "the font is awful"). Loading
/// is asynchronous (`FontFace.load()`), but the bytes are already in
/// memory - parsing 919 kB takes milliseconds, and Python start-up gives
/// plenty of slack.
m2w.loadFont = function () {
    try {
        var bytes = FS.readFile('/tahoma.ttf');
        var font = new FontFace('Tahoma', bytes.buffer);
        document.fonts.add(font);
        font.load().then(function () {
            console.log('m2w font: Tahoma loaded from tahoma.ttf');
        }).catch(function (e) {
            console.warn('m2w font: Tahoma failed to load - ' + e);
        });
    } catch (e) {
        console.warn('m2w font: no /tahoma.ttf in MEMFS (' + e + ') - '
                     + 'run tools/build_client_data.py; the UI will use the '
                     + 'browser fallback font');
    }
};

// ---------------------------------------------------------------------------
// Network: the WebSocket bridge (network_web.cpp)
// ---------------------------------------------------------------------------
m2w.options.add([
    { name: 'bridge', file: 'network_web.cpp',
      doc: 'WebSocket bridge address (bridge/): ws://host:port, wss://host, host:port or host; default 127.0.0.1:11496 locally, the page host otherwise' }
]);

/// `m2w.bridge` = {host, port, tls} once `m2w.bridgeStart` ran.
m2w.bridge = m2w.bridge || null;

/// Decides where the bridge is and tells SOCKFS the URL scheme. Runs once,
/// at the first `connect()`.
///
/// TWO DEPLOYMENTS, both real: (1) a LOCAL page (file://, localhost, 127.0.0.1) - the
/// bridge stands next to it on its own port 11496; (2) a page FROM A
/// SERVER (nginx) - the bridge has no address of its own, it goes through
/// the page's host and port and the web server forwards it, hence
/// `location.host` WITH the port. Over HTTPS `wss://` is required: the
/// browser refuses a plain WebSocket from an encrypted page.
///
/// An explicit address wins over everything: `?bridge=` (local pages only -
/// see m2w.options), then
/// `m2w.config.bridge` (written into
/// site/client.html by link_client.py from private/bridge.txt -
/// page on a hosting, proxy elsewhere). Accepted with or without
/// a scheme: "ws://host:port", "wss://host", "host:port", "host".
m2w.bridgeStart = function () {
    if (m2w.bridge) return;

    var tls = false;
    var host = '127.0.0.1';
    var port = 11496;

    try {
        var loc = globalThis.location;
        var local = (loc.protocol === 'file:') ||
                    /^(localhost|127\.0\.0\.1|\[::1\])$/.test(loc.hostname);
        tls = (loc.protocol === 'https:');

        if (local) {
            host = '127.0.0.1';
            port = 11496;
        } else {
            host = loc.hostname;
            // No port in the address means the scheme's default, not 11496
            // - the nginx-on-80/443 case.
            port = loc.port ? parseInt(loc.port, 10) : (tls ? 443 : 80);
        }

        var manual = m2w.options.get('bridge');
        if (!manual && m2w.config.bridge) manual = String(m2w.config.bridge);
        if (manual) {
            if (manual.indexOf('wss://') === 0) { tls = true;  manual = manual.slice(6); }
            else if (manual.indexOf('ws://') === 0) { tls = false; manual = manual.slice(5); }
            var colon = manual.lastIndexOf(':');
            if (colon > 0) {
                host = manual.slice(0, colon);
                port = parseInt(manual.slice(colon + 1), 10);
            } else {
                host = manual;
                port = tls ? 443 : 80;
            }
        }
    } catch (e) { }

    if (!(port > 0 && port < 65536)) port = 11496;

    m2w.bridge = { host: host, port: port, tls: tls };

    // SOCKFS builds the address itself but takes the scheme from here. It
    // must stay exactly "ws://" or "wss://": only then emscripten APPENDS
    // host, port and path; anything longer is used verbatim and every
    // connection would go to the same address without the target in the
    // path. SOCKFS copies `Module["websocket"]` into `SOCKFS.websocketArgs`
    // ONCE, at mount (start-up), and this runs at the first connect() -
    // so writing `Module["websocket"]` alone creates an object SOCKFS no
    // longer reads and the default "ws://" stays; on http no difference,
    // on https a SecurityError -> EHOSTUNREACH. Measured on the hosted
    // page: `SOCKFS.websocketArgs = {}`, `Module.websocket = {url:"wss://"}`,
    // different objects. Hence BOTH are written.
    var scheme = tls ? 'wss://' : 'ws://';
    if (typeof Module !== 'undefined') {
        Module['websocket'] = Module['websocket'] || {};
        Module['websocket']['url'] = scheme;
    }
    if (typeof SOCKFS !== 'undefined' && SOCKFS.websocketArgs)
        SOCKFS.websocketArgs['url'] = scheme;

    console.log('m2w network: bridge ' + scheme + host + ':' + port);
};

/// Registers `<bridge host>/to/<target>` in emscripten's name table and
/// returns the fake address it was given, packed like `sin_addr` (network
/// order: first octet in the lowest byte). 0 when the table refused.
m2w.addressViaBridge = function (target) {
    var k = m2w.bridge;
    var host = (k && k.host) ? k.host : '127.0.0.1';
    var ip = DNS.lookup_name(host + '/to/' + target);
    var a = ip.split('.');
    if (a.length !== 4) return 0;
    return ((+a[0]) | ((+a[1]) << 8) | ((+a[2]) << 16) | ((+a[3]) << 24)) | 0;
};

// ---------------------------------------------------------------------------
// In-game web browser (webbrowser_web.cpp): an <iframe> over the canvas
// ---------------------------------------------------------------------------
/// `m2w.webbrowser.frame` is the iframe, created on the first show.
m2w.webbrowser = m2w.webbrowser || { frame: null };

/// Creates the iframe if needed, places it at (left, top, right, bottom)
/// CSS pixels and loads `url`. Returns 1 when the frame exists; whether
/// the PAGE shows in it depends on the shop's headers (see the .cpp).
m2w.webbrowserShow = function (url, left, top, right, bottom) {
    var S = m2w.webbrowser;
    if (!globalThis.document) return 0;
    url = url ? m2w.safeUrl(url) : '';
    if (!url) return 0;

    if (!S.frame) {
        S.frame = document.createElement('iframe');
        S.frame.id = 'm2w_webbrowser';
        S.frame.style.position = 'absolute';
        S.frame.style.border = '0';
        // No `allow-same-origin` towards OUR page: the shop gets an origin
        // of its own and cannot reach into the client.
        S.frame.setAttribute('sandbox',
            'allow-scripts allow-forms allow-popups allow-top-navigation-by-user-activation');
        document.body.appendChild(S.frame);
    }

    m2w.webbrowserMove(left, top, right, bottom);
    S.frame.style.display = 'block';
    S.frame.src = url;
    return 1;
};

/// Moves and resizes the frame to (left, top, right, bottom) CSS pixels;
/// nothing before the first show.
m2w.webbrowserMove = function (left, top, right, bottom) {
    var S = m2w.webbrowser;
    if (!S.frame) return;
    S.frame.style.left   = left + 'px';
    S.frame.style.top    = top + 'px';
    S.frame.style.width  = (right - left) + 'px';
    S.frame.style.height = (bottom - top) + 'px';
};

/// Hides the frame and unloads the page on purpose: a hidden frame would
/// live on, play sound and keep the shop's connections.
m2w.webbrowserHide = function () {
    var S = m2w.webbrowser;
    if (!S.frame) return;
    S.frame.style.display = 'none';
    S.frame.src = 'about:blank';
};

/// Removes the frame from the page for good (the next show creates a new one).
m2w.webbrowserDestroy = function () {
    var S = m2w.webbrowser;
    if (!S.frame) return;
    if (S.frame.parentNode) S.frame.parentNode.removeChild(S.frame);
    S.frame = null;
};

// ---------------------------------------------------------------------------
// Corpus: game data streamed on demand (webfs_web.cpp)
// ---------------------------------------------------------------------------
m2w.options.add([
    { name: 'corpus',     file: 'webfs_web.cpp',
      doc: 'address of the corpus directory (manifest.bin + <hash>.bin chunks); default corpus/ next to the page' },
    { name: 'corpusMB',   file: 'webfs_web.cpp', site: true,
      doc: 'chunk memory budget in MB (default by device memory: 512 from 8 GB, 256 from 4 GB or unknown, else 96)' },
    { name: 'noprefetch', file: 'webfs_web.cpp', site: true,
      doc: '1 = no prefetch in the recorded order (A/B measurement)' },
    { name: 'nofetch',    file: 'site/preload.js', site: true,
      doc: '1 = no background download of the whole corpus (read by the page itself, which loads before client.js)' }
]);

/// What the page (site/preload.js) and the console
/// (tools/chunk_order.py) read about the corpus - one object since
/// Phase E (before: loose globals `m2w_all_chunks`,
/// `m2w_corpus_url_js`, `m2w_chunk_order`, `m2w_fetch_count`):
/// `url` corpus address, `all` every chunk hash, `order` chunk indices in
/// order of first use, `fetchCount` synchronous downloads so far.
/// The prefetch progress is `m2w.prefetch` (set by `m2w.prefetchStart`).
m2w.corpus = m2w.corpus || { url: '', all: [], order: [], fetchCount: 0 };
m2w.prefetch = m2w.prefetch || null;

/// Downloads `url` SYNCHRONOUSLY and returns a Uint8Array, or null: a
/// synchronous XMLHttpRequest on the main thread
/// - not Asyncify, not SharedArrayBuffer, both cost more than needed, and
/// it does NOT change the order of game code, which assumes the file is
/// ready right after `fopen`. `responseType = 'arraybuffer'` is ILLEGAL on
/// a synchronous request, so the bytes come as text through
/// `charset=x-user-defined`, which maps every byte to U+F700+byte and
/// `& 0xFF` gives the byte back - for 0x80-0x9F too. (ISO-8859-1, tried
/// first, is aliased to windows-1252 by the WHATWG standard and
/// remaps 0x80-0x9F: byte 0x94 came out as 0x1D.)
///
/// `expected` > 0 is the chunk size from the manifest: the text API does
/// BOM sniffing despite the charset, and a chunk starting with FE FF /
/// FF FE / EF BB BF came back decoded as UTF-16/UTF-8 with another length
/// (measured: chunk 195, 4194304 B -> 2097151 "characters", 44 files read
/// from shifted places). Refusing beats garbage.
/// Compared with the manifest size, not Content-Length (under gzip that
/// counts bytes on the wire). build_corpus.py makes sure no chunk starts
/// with a BOM; this is the second line.
m2w.fetchSync = function (url, expected) {
    try {
        var xhr = new XMLHttpRequest();
        xhr.open('GET', url, false);
        xhr.overrideMimeType('text/plain; charset=x-user-defined');
        xhr.send(null);
        if (xhr.status !== 200 && xhr.status !== 0) return null;
        var text = xhr.responseText;
        var n = text.length;
        ++m2w.corpus.fetchCount;
        if (expected > 0 && n !== expected) {
            console.error('m2w corpus: ' + url + ' - manifest ' + expected + ' B, but text ' + n +
                          ' characters (BOM sniffing?) - REFUSED instead of garbage');
            return null;
        }
        var bytes = new Uint8Array(n);
        for (var i = 0; i < n; ++i) bytes[i] = text.charCodeAt(i) & 0xFF;
        return bytes;
    } catch (e) {
        return null;
    }
};

/// Chunk memory on the page side. Why: the synchronous XHR was
/// the ONLY road - the corpus then had 477 chunks and we kept sixteen, so in the
/// world a miss was the rule and every miss a full blocking round trip:
/// stutter on entering new terrain. Two things were missing: PERSISTENT
/// memory (Cache Storage - a chunk stays on disk between sessions; needs a
/// secure origin: 127.0.0.1 is one, a LAN address over plain http is not)
/// and BACKGROUND fetches (a chunk fetched
/// asynchronously costs no frame; when the game asks, it is already here).
///
/// `m2w.chunks`: `map` name -> Uint8Array (insertion order = age),
/// `inFlight` names already requested, `bytes` held (reservations of the
/// prefetch included), `budget`, counters `fromMemory` / `fromStorage` /
/// `fromNetwork`, `used` = names already handed to the game (evicted
/// first), `background` = fetches started from `Get()` in flight (limit 2),
/// `storage` = promise of the Cache Storage or null.
m2w.chunks = m2w.chunks || null;

/// Creates the chunk cache once (`m2w.chunks`, budget `budgetMB` MB) with its
/// eviction rule and opens the Cache Storage `m2w-corpus` (null where the
/// origin has none - then chunks do not survive the session).
m2w.chunkCacheStart = function (budgetMB) {
    if (m2w.chunks) return;
    // Measurement: `performance.getEntriesByType('resource')` for every
    // chunk (the default buffer of 250 stops half-way through loading).
    try { performance.setResourceTimingBufferSize(4000); } catch (e) { }
    var state = {
        map: new Map(),
        inFlight: new Set(),
        bytes: 0,
        budget: budgetMB * 1024 * 1024,
        fromMemory: 0,
        fromStorage: 0,
        fromNetwork: 0,
        used: new Set(),
        background: 0
    };
    // Makes room in the budget: FIRST the oldest already used, only then the
    // oldest unused (e.g. prefetched, not yet read). `protectedName` is
    // what is being inserted. Bytes reserved by the prefetch (no key in the
    // map) are in `state.bytes` and come back when the chunk arrives -
    // hence the loop also ends on an empty map.
    /// Evicts toward the budget: first the oldest chunks the game
    /// already used, then the oldest unused; never `protectedName` or the last
    /// chunk.
    state.evict = function (protectedName) {
        var pass = 0;
        while (state.bytes > state.budget && state.map.size > 1 && pass < 2) {
            var found = false;
            var it = state.map.keys();
            for (var r = it.next(); !r.done; r = it.next()) {
                var k = r.value;
                if (k === protectedName) continue;
                if (pass === 0 && !state.used.has(k)) continue;
                state.bytes -= state.map.get(k).length;
                state.map.delete(k);
                state.used.delete(k);
                found = true;
                break;
            }
            if (!found) ++pass;
        }
    };
    m2w.chunks = state;

    state.storage = new Promise(function (resolve) {
        if (typeof caches === 'undefined' || !caches || !caches.open) {
            console.warn('m2w corpus: no Cache Storage on this origin ' +
                         '(needs https or localhost) - chunks will not ' +
                         'survive between sessions');
            resolve(null);
            return;
        }
        caches.open('m2w-corpus').then(function (c) { resolve(c); })
                                  .catch(function () { resolve(null); });
    });
};

/// SYNCHRONOUS read from the page memory: the chunk's bytes or null. This
/// is the whole gain of background fetching - a chunk already here costs
/// the game no round trip.
m2w.chunkFromCache = function (name) {
    var state = m2w.chunks;
    if (!state) return null;
    var data = state.map.get(name);
    if (!data) return null;
    ++state.fromMemory;
    state.used.add(name);
    if (m2w.prefetch) m2w.prefetch.lastProgress = performance.now();
    return data;
};

/// Puts a chunk the game just read (blocking fetch) into the page memory,
/// marked as used, and keeps the budget.
m2w.chunkCachePut = function (name, bytes) {
    var state = m2w.chunks;
    if (!state || !bytes || !bytes.length) return;
    if (state.map.has(name)) return;
    state.map.set(name, bytes);
    state.bytes += bytes.length;
    state.used.add(name);
    state.evict(name);
};

/// Fetches a chunk IN THE BACKGROUND - Cache Storage first, then the
/// network - into the page memory. UPPER BOUND IN FLIGHT: the synchronous road never yields, so successive `Get()`
/// calls in one frame kept adding 3 fetches each - 52 measured at once;
/// the browser has 6 sockets per server, the fetches held them with bodies
/// nobody read, and the synchronous XHR waited for a socket exactly 20.0 s
/// (7 x 20 s = 140 of 145 s of loading). Two from here + two from the
/// prefetch (a separate counter so neither starves the other) = 4 of 6;
/// the XHR always has one free.
m2w.chunkInBackground = function (url, name) {
    var state = m2w.chunks;
    if (!state) return;
    if (state.map.has(name) || state.inFlight.has(name)) return;
    if (state.bytes > state.budget) return;
    if (state.background >= 2) return;
    state.inFlight.add(name);
    ++state.background;

    /// Stores the fetched chunk (unless it arrived meanwhile), evicts to budget,
    /// counts where it came from and frees the background slot.
    var put = function (buffer, fromStorage) {
        var data = new Uint8Array(buffer);
        if (!state.map.has(name)) {
            state.map.set(name, data);
            state.bytes += data.length;
            state.evict(name);
        }
        if (fromStorage) ++state.fromStorage; else ++state.fromNetwork;
        state.inFlight.delete(name);
        --state.background;
    };

    state.storage.then(function (storage) {
        var stored = storage ? storage.match(url) : Promise.resolve(null);
        return Promise.resolve(stored).then(function (response) {
            if (response) return response.arrayBuffer().then(function (b) { put(b, true); });
            return fetch(url).then(function (response2) {
                if (!response2 || !response2.ok) throw new Error('HTTP');
                if (storage) { try { storage.put(url, response2.clone()); } catch (e) { } }
                return response2.arrayBuffer().then(function (b) { put(b, false); });
            });
        });
    }).catch(function () { state.inFlight.delete(name); --state.background; });
};

/// PREFETCH IN THE RECORDED ORDER.
/// `<corpus>/order.txt` (tools/chunk_order.py) lists chunk
/// hashes in the order the game reached for them at login and entering
/// the village (recorded once through `m2w.corpus.order`). From
/// page start they are fetched in that order, two at a time, into the page
/// memory and Cache Storage.
///
/// SLIDING WINDOW: the HTTP cache proved UNRELIABLE (measured: after 400 MB
/// fetched in 2 s the synchronous XHR still went to the network despite
/// `immutable`), and Cache Storage cannot be read synchronously. The only
/// thing that spares the game is a chunk lying in `map` BEFORE it asks.
/// So the prefetch downloads nothing it has no budget for: it waits until
/// the game USES (reads from memory) the oldest chunks, evicts them, and
/// only then fetches the next. With the game reading in the same order the
/// prefetch stays one budget (m2w.corpusBudget: 96-512 MB = 24-128 chunks)
/// ahead. When the game left the recorded path and used nothing for 15 s, the
/// oldest chunk is evicted anyway - or the prefetch would stand on unwanted
/// chunks all session.
/// `?noprefetch=1` disables it. Progress: `m2w.prefetch` and the
/// `Module.setStatus` bar before entering the game.
m2w.prefetchStart = function (base) {
    var state = m2w.chunks;
    if (!state || m2w.prefetch) return;
    if (m2w.options.get('noprefetch') === '1') {
        console.log('m2w corpus: prefetch DISABLED (?noprefetch=1)');
        return;
    }
    var CHUNK = 4 * 1048576;
    var w = m2w.prefetch = {
        total: 0, done: 0, toMemory: 0, fromStorage: 0, errors: 0,
        waits: 0, forcedEvictions: 0, start: performance.now(), end: 0,
        lastProgress: performance.now()
    };

    // Makes room for one chunk by evicting the OLDEST ALREADY USED; an
    // unused one is left alone, the game will still come for it.
    /// Evicts OLDEST ALREADY USED chunks until one more chunk (CHUNK bytes) fits;
    /// false when that is impossible without touching unused ones.
    var makeRoom = function () {
        var it = state.map.keys();
        while (state.bytes + CHUNK > state.budget) {
            var k = it.next();
            if (k.done) return false;
            if (!state.used.has(k.value)) continue;
            state.bytes -= state.map.get(k.value).length;
            state.map.delete(k.value);
            state.used.delete(k.value);
        }
        return true;
    };
    // Waits for room and RESERVES it (`state.bytes += CHUNK`), so parallel
    // fetches do not count on the same free slot.
    /// Resolves once room for one chunk is found and RESERVED; after 15 s with no
    /// progress evicts the oldest chunk anyway, otherwise retries every 200 ms.
    var waitForRoom = function () {
        if (!makeRoom()) {
            if (performance.now() - w.lastProgress > 15000 && state.map.size) {
                var oldest = state.map.keys().next().value;
                state.bytes -= state.map.get(oldest).length;
                state.map.delete(oldest);
                ++w.forcedEvictions;
                w.lastProgress = performance.now();
            }
            if (!makeRoom()) {
                ++w.waits;
                return new Promise(function (resolve) { setTimeout(resolve, 200); })
                    .then(waitForRoom);
            }
        }
        state.bytes += CHUNK;
        return Promise.resolve();
    };
    /// Stores a prefetched chunk in place of its reservation (unless it arrived
    /// meanwhile) and marks progress.
    var put = function (name, buffer) {
        state.bytes -= CHUNK;
        if (state.map.has(name)) return;
        var data = new Uint8Array(buffer);
        state.map.set(name, data);
        state.bytes += data.length;
        ++w.toMemory;
        w.lastProgress = performance.now();
    };
    /// Every tenth chunk (and at the end) logs the prefetch progress and updates
    /// the page status bar until the game starts.
    var report = function () {
        if ((w.done % 10) !== 0 && w.done !== w.total) return;
        console.log('m2w corpus: prefetch ' + w.done + '/' + w.total +
                    ' (in memory ' + w.toMemory + ', from storage ' + w.fromStorage +
                    ', errors ' + w.errors + ', waits for room ' + w.waits + ', ' +
                    Math.round((performance.now() - w.start) / 100) / 10 + ' s)');
        try {
            var s = document.getElementById('status');
            if (s && s.className !== 'done' && typeof Module !== 'undefined' &&
                Module.setStatus && w.done !== w.total)
                Module.setStatus('Game data: ' + w.done + ' / ' + w.total + ' chunks...');
        } catch (e) { }
    };

    fetch(base + 'order.txt', { cache: 'no-store' }).then(function (o) {
        if (!o.ok) throw new Error('HTTP ' + o.status);
        return o.text();
    }).then(function (t) {
        var hashes = t.match(/[0-9a-f]{32}/g) || [];
        w.total = hashes.length;
        return state.storage.then(function (storage) {
            var i = 0;
            /// Fetches the next chunk of the list (from Cache Storage or the network),
            /// then calls itself; two of these run in parallel.
            var one = function () {
                if (i >= hashes.length) return Promise.resolve();
                var name = hashes[i++] + '.bin';
                var url = base + name;
                if (state.map.has(name) || state.inFlight.has(name)) {
                    ++w.done; report(); return one();
                }
                return waitForRoom().then(function () {
                    if (state.map.has(name) || state.inFlight.has(name)) {
                        state.bytes -= CHUNK;
                        return;
                    }
                    state.inFlight.add(name);
                    var p = storage ? storage.match(url) : Promise.resolve(null);
                    return Promise.resolve(p).then(function (response) {
                        if (response) {
                            ++w.fromStorage;
                            return response.arrayBuffer().then(function (b) { put(name, b); });
                        }
                        return fetch(url).then(function (response2) {
                            if (!response2 || !response2.ok) throw new Error('HTTP ' + (response2 && response2.status));
                            if (storage) { try { storage.put(url, response2.clone()); } catch (e) { } }
                            return response2.arrayBuffer().then(function (b) { put(name, b); });
                        });
                    }).catch(function (e) {
                        ++w.errors;
                        state.bytes -= CHUNK;
                        console.warn('m2w corpus: prefetch ' + name + ' - ' + e);
                    }).then(function () { state.inFlight.delete(name); });
                }).then(function () {
                    ++w.done; report();
                    return one();
                });
            };
            return Promise.all([one(), one()]);
        });
    }).then(function () {
        w.end = performance.now();
    }).catch(function (e) {
        console.warn('m2w corpus: no order.txt - no prefetch (' + e + ')');
    });
};

/// Hands the page the list of all chunk hashes and the corpus address
/// (preload.js downloads the rest into Cache Storage, from which the next
/// start's prefetch reads it into memory. The Service
/// Worker does NOT see the synchronous XHR, see below).
m2w.chunkList = function (list, base) {
    m2w.corpus.all = list.split('\n').filter(function (x) { return x.length === 32; });
    m2w.corpus.url = base;
};

/// Counters for the report: 0 from memory, 1 from storage, 2 from network,
/// 3 chunks held, 4 MB held.
m2w.chunkCounter = function (what) {
    var s = m2w.chunks;
    if (!s) return 0;
    if (what === 0) return s.fromMemory;
    if (what === 1) return s.fromStorage;
    if (what === 2) return s.fromNetwork;
    if (what === 3) return s.map.size;
    return (s.bytes / 1048576) | 0;
};

/// Chunk memory budget in MB (`?corpusMB=` wins; otherwise by the device's
/// memory). Until the measurement it was 96.
/// Measured with a headless Edge, login + village,
/// 90 s: the game uses ~102 chunks (~408 MB), and with 96 MB it evicted
/// chunks it needed again a moment later - 138 BLOCKING downloads on a
/// SECOND start (89 from the network, 4.8 s of frozen main thread on
/// localhost); 256 MB -> 75 (62 from the network, 2.8 s); 512 MB -> 22
/// (0 from the network, 0.6 s), also on a first start 22 instead of 137.
/// A Service Worker does not help here: it does not see the synchronous
/// XHR at all. `navigator.deviceMemory` is the RAM in GB
/// rounded down and capped at 8 (Chromium; Firefox and Safari give none).
m2w.corpusBudget = function () {
    var v = m2w.options.get('corpusMB');
    if (v) { var i = parseInt(v, 10); if (i > 0) return i; }
    var gb = (typeof navigator !== 'undefined') ? navigator.deviceMemory : undefined;
    if (gb >= 8) return 512;
    if (gb >= 4) return 256;
    if (gb > 0) return 96;
    return 256;
};

/// Corpus address with a trailing slash (`?corpus=`, default corpus/).
m2w.corpusUrl = function () {
    var s = m2w.options.get('corpus') || 'corpus/';
    if (s.length && s[s.length - 1] !== '/') s += '/';
    return s;
};

/// Records the first use of a chunk (tools/chunk_order.py reads
/// `m2w.corpus.order` from the console after a session).
m2w.chunkUsed = function (index) {
    m2w.corpus.order.push(index);
};

// ---------------------------------------------------------------------------
// Text: 2D canvases behind GDI (platform_text.cpp)
// ---------------------------------------------------------------------------
// `m2w.canvases[i]` = {c: canvas, x: 2D context}; the fallback road when a
// font has no baked glyphs (the main road pastes GDI bitmaps in C++).
m2w.canvases = m2w.canvases || [];

/// Creates a canvas of the given size and returns its index.
m2w.textCanvasCreate = function (width, height) {
    var c = (typeof OffscreenCanvas !== 'undefined')
            ? new OffscreenCanvas(width, height)
            : document.createElement('canvas');
    c.width = width; c.height = height;
    m2w.canvases.push({ c: c, x: c.getContext('2d', { willReadFrequently: true }) });
    return m2w.canvases.length - 1;
};

/// Sets the CSS font of text canvas `id` (baseline at the top).
m2w.textCanvasFont = function (id, css) {
    var p = m2w.canvases[id];
    p.x.font = css;
    p.x.textBaseline = 'top';
};

/// Advance width of `text` in CSS pixels, in the canvas font.
m2w.textMeasure = function (id, text) {
    return m2w.canvases[id].x.measureText(text).width;
};

/// Width, left and right bounding box of `text` - three numbers from one
/// `TextMetrics` (one boundary crossing instead of three).
m2w.textMeasureThree = function (id, text) {
    var m = m2w.canvases[id].x.measureText(text);
    return [m.width,
            (m.actualBoundingBoxLeft === undefined) ? 0 : m.actualBoundingBoxLeft,
            (m.actualBoundingBoxRight === undefined) ? m.width : m.actualBoundingBoxRight];
};

/// REAL KERNING: cumulative width after every prefix of `text`,
/// so the difference of two neighbours is the width of a character IN ITS
/// NEIGHBOURHOOD - measured in isolation, "T" next to "y" and "T" next to
/// "l" would come out the same, and the strings looked "crooked" (user).
m2w.textMeasurePrefixes = function (id, text, capacity) {
    var n = Math.min(text.length, capacity);
    var p = m2w.canvases[id];
    var out = new Array(n);
    for (var i = 0; i < n; ++i) out[i] = p.x.measureText(text.slice(0, i + 1)).width;
    return out;
};

/// Right edge of `text`'s ink (`actualBoundingBoxRight`), or its width where
/// the browser does not report it.
m2w.textBboxRight = function (id, text) {
    var m = m2w.canvases[id].x.measureText(text);
    return (m.actualBoundingBoxRight === undefined) ? m.width : m.actualBoundingBoxRight;
};

/// Ink height of `text` (ascent + descent of its bounding box).
m2w.textHeight = function (id, text) {
    var m = m2w.canvases[id].x.measureText(text);
    var a = (m.actualBoundingBoxAscent === undefined) ? 0 : m.actualBoundingBoxAscent;
    var d = (m.actualBoundingBoxDescent === undefined) ? 0 : m.actualBoundingBoxDescent;
    return a + d;
};

/// FULL LINE height of the font, NOT of a particular string:
/// `GrpFontTexture::UpdateCharacterInfomation` packs every character into
/// the atlas in a cell as high as `GetTextExtentPoint32` says. Real GDI
/// returns the line height (ascent + descent + internal leading); we used
/// to return the nominal size (`lfHeight`), without room for descenders,
/// so with `textBaseline='top'` a "y" spilled into the NEXT atlas row and
/// showed up as a stray dot under an unrelated string (user report).
/// `fontBoundingBoxAscent/Descent` measure the whole font; the "Mjpqy"
/// probe is the fallback for browsers without those fields.
m2w.textLineHeight = function (id) {
    var m = m2w.canvases[id].x.measureText('Mjpqy');
    if (m.fontBoundingBoxAscent !== undefined && m.fontBoundingBoxDescent !== undefined)
        return m.fontBoundingBoxAscent + m.fontBoundingBoxDescent;
    var a = (m.actualBoundingBoxAscent === undefined) ? 0 : m.actualBoundingBoxAscent;
    var d = (m.actualBoundingBoxDescent === undefined) ? 0 : m.actualBoundingBoxDescent;
    return a + d;
};

/// Draws `text` at (x, y); with `opaque` the background is filled first,
/// from the text down to the bottom of the canvas (GDI OPAQUE semantics).
m2w.textDraw = function (id, x, y, text, colour, background, opaque) {
    var p = m2w.canvases[id];
    if (opaque) {
        var w = p.x.measureText(text).width;
        p.x.fillStyle = background;
        p.x.fillRect(x, y, w, p.c.height - y);
    }
    p.x.fillStyle = colour;
    p.x.fillText(text, x, y);
};

/// Reads the rectangle [x0, x1) x [y0, y1) of the canvas into the DIB
/// (`target` = wasm address, `stride` = DIB width) as BGRA - the step that
/// hands the picture to TMP4 under the pointer from `CreateDIBSection`.
///
/// Only the touched rectangle: the first version copied the whole
/// 512x512 atlas for EVERY new character - a `getImageData` plus about a
/// million writes per letter, thirty new chat characters = thirty million
/// writes in one frame, stutter at every new name.
/// Only columns of THIS character: a full-width copy after every
/// `TextOutW` let a "y" (ink one pixel LEFT of its origin) permanently
/// overwrite the "L" packed before it - the atlas has no gutter.
/// Anti-aliasing threshold: `getImageData` returns
/// un-premultiplied colour, so a pixel barely touched is (255,255,255,a=2)
/// and the engine's "blue != 0" binarisation turned every faint edge into
/// a full dot; pixels under alpha 8 are dropped here (lowered from 64 in
/// 291h, when the dots proved to be the overwrite above, not the halo).
m2w.textReadRows = function (id, target, stride, x0, x1, y0, y1) {
    var p = m2w.canvases[id];
    if (y1 <= y0 || x1 <= x0) return;
    if (x0 < 0) x0 = 0;
    if (x1 > stride) x1 = stride;
    if (x1 <= x0) return;
    var w = x1 - x0;
    var d = p.x.getImageData(x0, y0, w, y1 - y0).data;
    var threshold = 8;
    // GDI keeps pixels as BGRA, the canvas as RGBA - the swap is here,
    // because TMP4 reads these bytes directly.
    for (var row = y0; row < y1; ++row) {
        var cb = target + (row * stride + x0) * 4;
        var db = (row - y0) * w * 4;
        for (var col = 0; col < w; ++col) {
            var a = d[db + col * 4 + 3];
            if (a < threshold) {
                HEAPU8[cb + col * 4 + 0] = 0;
                HEAPU8[cb + col * 4 + 1] = 0;
                HEAPU8[cb + col * 4 + 2] = 0;
                HEAPU8[cb + col * 4 + 3] = 0;
                continue;
            }
            HEAPU8[cb + col * 4 + 0] = d[db + col * 4 + 2];
            HEAPU8[cb + col * 4 + 1] = d[db + col * 4 + 1];
            HEAPU8[cb + col * 4 + 2] = d[db + col * 4 + 0];
            HEAPU8[cb + col * 4 + 3] = a;
        }
    }
};

// ---------------------------------------------------------------------------
// Sound: Web Audio behind CSoundManager (sound_web.cpp)
// ---------------------------------------------------------------------------
// Deliberately thin: every NUMBER (volume curve, distance scales, music
// state machine, slot allocation) stays in C++ where it can be compared line
// by line with `milesLib/SoundManager.cpp`; here is only what C++ cannot do.
//
// `m2w.sound`: `ctx` = the AudioContext (null when the browser has none),
// `buffers` = Map crc -> AudioBuffer | null (decoding) | false (failed),
// `slots[i]` = a permanent PAIR of nodes (gain + panner) plus the one-shot
// source playing in it; `gesture` = the first user gesture has happened.
//
// A slot is a permanent pair because `AudioBufferSourceNode` is single-use
// (a Miles `HSAMPLE` could be restarted), so a source is created at every
// play and connected to the pair. `IsDone` counts as Miles did: the slot is
// free when nothing plays in it and nothing waits to be decoded.
m2w.sound = m2w.sound || null;

/// Starts a buffer in a slot; shared by an immediate play and by a decode
/// that has just finished. `generation` guards the `onended` callback: a
/// slot re-allocated in the meantime belongs to somebody else.
m2w.soundStartSource = function (slot, buffer) {
    var S = m2w.sound;
    var s = S.slots[slot];

    if (s.source) { try { s.source.stop(); } catch (e) { } s.source = null; }

    var src = S.ctx.createBufferSource();
    src.buffer = buffer;
    // Miles: `iLoopCount` 0 means "forever", 1 "once", n "n times".
    src.loop = (s.loops === 0);
    src.connect(s.spatial ? s.panner : s.gain);

    var mine = s.generation;
    var left = s.loops;
    /// When a play finishes: plays again while repeats remain (Miles loop count),
    /// otherwise frees the slot - unless the slot was reused meanwhile.
    src.onended = function () {
        if (s.generation !== mine) return;   // slot already given to somebody else
        if (!src.loop && --left > 0) { m2w.soundStartSource(slot, buffer); return; }
        s.source = null;
    };

    s.source = src;
    if (S.ctx.state === 'suspended') S.ctx.resume();
    src.start();
};

/// Creates the context and `slots` node pairs. `equalpower` panning IS the
/// provider TMP4 chose ("Miles Fast 2D Positional Audio": pan and volume,
/// no HRTF); `refDistance` 1 because C++ divides positions by the sound
/// scale (200) first - see sound_web.cpp, "Design".
m2w.soundStart = function (slots) {
    if (m2w.sound) return;

    var S = { ctx: null, buffers: new Map(), slots: [], gesture: false };
    m2w.sound = S;

    var Context = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (!Context) return;
    S.ctx = new Context();

    for (var i = 0; i < slots; ++i) {
        var g = S.ctx.createGain();
        var p = S.ctx.createPanner();
        p.panningModel = 'equalpower';   // "Miles Fast 2D Positional Audio"
        p.distanceModel = 'inverse';
        p.refDistance = 1;               // see the note on the constant 200
        p.rolloffFactor = 1;
        p.connect(g);
        g.connect(S.ctx.destination);
        S.slots.push({
            gain: g, panner: p, source: null,
            waiting: 0,      // crc of the file being waited for (0 = none)
            generation: 0,   // grows at every play and stop
            loops: 1,
            spatial: 0
        });
    }

    // The browser will not play before the first user gesture. Miles had no
    // such notion - a platform requirement, not a choice.
    /// The first user gesture: remembers it and resumes the suspended audio
    /// context (browsers allow sound only after one).
    var wake = function () {
        if (S.gesture) return;
        S.gesture = true;
        if (S.ctx && S.ctx.state === 'suspended') S.ctx.resume();
    };
    ['pointerdown', 'keydown', 'touchstart'].forEach(function (n) {
        globalThis.addEventListener(n, wake, { once: true, passive: true });
    });
};

/// Hands the bytes of a file to the decoder. 1 = buffer ready, 0 = decoding
/// (slots that wait for this crc start when it finishes), -1 = cannot.
/// `bytes` must be a COPY: `decodeAudioData` detaches the buffer it gets
/// and a piece of the wasm heap must never be detached.
m2w.soundLoad = function (crc, bytes) {
    var S = m2w.sound;
    if (!S || !S.ctx) return -1;

    var known = S.buffers.get(crc);
    if (known !== undefined) return (known === null) ? 0 : (known === false ? -1 : 1);

    S.buffers.set(crc, null);   // null = in progress

    S.ctx.decodeAudioData(bytes, function (buffer) {
        S.buffers.set(crc, buffer);
        for (var i = 0; i < S.slots.length; ++i) {
            var s = S.slots[i];
            if (s.waiting === crc) { s.waiting = 0; m2w.soundStartSource(i, buffer); }
        }
    }, function () {
        S.buffers.set(crc, false);   // false = tried and failed
        for (var i = 0; i < S.slots.length; ++i)
            if (S.slots[i].waiting === crc) S.slots[i].waiting = 0;
    });

    return 0;
};

/// Plays a file in a slot: now if decoded, when the decode finishes if
/// not yet, never if unknown or broken.
m2w.soundPlay = function (slot, crc, loops, spatial) {
    var S = m2w.sound;
    if (!S || !S.ctx) return;
    var s = S.slots[slot];
    if (!s) return;

    ++s.generation;
    s.loops = loops;
    s.spatial = spatial;
    s.waiting = 0;

    var buffer = S.buffers.get(crc);
    if (buffer === undefined || buffer === false) return;   // unknown or broken
    if (buffer === null) { s.waiting = crc; return; }       // still decoding
    m2w.soundStartSource(slot, buffer);
};

/// Stops slot `slot` at once and forgets a pending start (bumps the
/// generation, so a late `onended` does nothing).
m2w.soundStop = function (slot) {
    var S = m2w.sound;
    if (!S) return;
    var s = S.slots[slot];
    if (!s) return;
    ++s.generation;
    s.waiting = 0;
    if (s.source) { try { s.source.stop(); } catch (e) { } s.source = null; }
};

/// 1 when nothing plays in the slot and nothing waits for a decode.
m2w.soundSlotFree = function (slot) {
    var S = m2w.sound;
    if (!S) return 1;
    var s = S.slots[slot];
    if (!s) return 1;
    return (s.source === null && s.waiting === 0) ? 1 : 0;
};

/// Sets the slot's gain (0..1) from now on.
m2w.soundVolume = function (slot, volume) {
    var S = m2w.sound;
    if (!S || !S.ctx) return;
    var s = S.slots[slot];
    if (s) s.gain.gain.setValueAtTime(volume, S.ctx.currentTime);
};

/// Source position relative to the listener (C++ already divided by the
/// scale and flipped Z).
m2w.soundPosition = function (slot, x, y, z) {
    var S = m2w.sound;
    if (!S) return;
    var s = S.slots[slot];
    if (!s) return;
    if (s.panner.positionX) {
        s.panner.positionX.value = x;
        s.panner.positionY.value = y;
        s.panner.positionZ.value = z;
    } else {
        s.panner.setPosition(x, y, z);   // older form of the same
    }
};

/// Listener position alone; TMP4 keeps it apart from the direction
/// (`SetListenerPosition` vs `SetListenerDirection`) and calls them from
/// different places, so here too they are two functions.
m2w.soundListenerPosition = function (x, y, z) {
    var S = m2w.sound;
    if (!S || !S.ctx) return;
    var L = S.ctx.listener;
    if (L.positionX) {
        L.positionX.value = x; L.positionY.value = y; L.positionZ.value = z;
    } else {
        L.setPosition(x, y, z);
    }
};

/// Listener direction alone - the counterpart of `AIL_set_3D_orientation`.
m2w.soundListenerDirection = function (fx, fy, fz, ux, uy, uz) {
    var S = m2w.sound;
    if (!S || !S.ctx) return;
    var L = S.ctx.listener;
    if (L.forwardX) {
        L.forwardX.value = fx; L.forwardY.value = fy; L.forwardZ.value = fz;
        L.upX.value = ux; L.upY.value = uy; L.upZ.value = uz;
    } else {
        L.setOrientation(fx, fy, fz, ux, uy, uz);
    }
};

// ---------------------------------------------------------------------------
// IME: focus of the hidden text element (ime_web.cpp)
// ---------------------------------------------------------------------------
// The browser composes characters (CJK input methods) only while a text
// element has focus. The design is a hidden element `m2w_ime`
// over the canvas whose `composition*` and `paste` events call the
// `M2W_Ime*` exports with `Module.ccall`; NO PAGE CREATES THAT ELEMENT
// and no such listener exists yet (git grep), so today this is a
// no-op and characters reach the game only as `WM_CHAR` from `keydown`
// (`m2w.eventsStart`). `EnableIME` may run before the page is ready, so a
// missing element is not an error.
/// Focuses (`enabled`) or blurs the `m2w_ime` element; nothing when the page
/// has none (today no page does).
m2w.imeEnable = function (enabled) {
    var e = globalThis.document && document.getElementById('m2w_ime');
    if (!e) return;
    if (enabled) e.focus(); else e.blur();
};

// ---------------------------------------------------------------------------
// Loop (win32_compat.cpp)
// ---------------------------------------------------------------------------
m2w.options.add([
    { name: 'fps', file: 'win32_compat.cpp', site: true,
      doc: 'artificial frame limit N for tests: loop turns faster than 1000/N ms are skipped, as on a slow monitor' },
    { name: 'events', file: 'win32_compat.cpp',
      doc: 'loop = the old road of the event frame counter, one event frame per loop turn, for A/B (stage_port.py)' }
]);

/// `?fps=N` as a number, 0 when absent or not positive.
m2w.fpsLimit = function () {
    var f = parseFloat(m2w.options.get('fps'));
    return (f > 0) ? f : 0;
};

/// 1 when `?events=loop` is in the address.
m2w.eventsFromLoop = function () {
    return m2w.options.get('events') === 'loop' ? 1 : 0;
};

// ---------------------------------------------------------------------------
// Forest (speedtree_web.cpp)
// ---------------------------------------------------------------------------
m2w.options.add([
    { name: 'forest', file: 'speedtree_web.cpp',
      doc: 'tree diagnostics 1-5: 1 draw every instance, 2 no alpha/fog/culling, 3 no depth test, 4 trees after the terrain, 5 every tree 25 m in front of the camera; gl_device.cpp writes /las.txt' }
]);

// ---------------------------------------------------------------------------
// GL device render switches (gl_*.cpp)
// ---------------------------------------------------------------------------
m2w.options.add([
    { name: 'tnl', file: 'gl_device.cpp',
      doc: '0 = software vertex transformation (VertexShaderVersion 0, the STP terrain road) to compare with the default hardware road' },
    { name: 'noflip', file: 'gl_render_states.cpp',
      doc: '1 switches the Y flip of drawing to a texture off (A/B test); window.m2w_no_reflection overrides it live' },
    { name: 'nocustom', file: 'gl_buffers.cpp',
      doc: '1 = characters through the fixed-function pipeline instead of road B (experiment)' },
    { name: 'nocull', file: 'gl_buffers.cpp',
      doc: '1 = no face culling on road B (experiment)' },
    { name: 'shadow', file: 'gl_buffers.cpp',
      doc: 'terrain shadow pass experiment: 0 skip the pass, 1/2/3 pretend no texture on stage 0/1/both' }
]);

// ---------------------------------------------------------------------------
// Skinning (granny_pose.cpp)
// ---------------------------------------------------------------------------
m2w.options.add([
    { name: 'skinshortcut', file: 'granny_pose.cpp',
      doc: '0 switches the full-weight skinning shortcut off, to measure both roads in one machine state' }
]);

// ---------------------------------------------------------------------------
// Probes without a server (custom_draw.cpp)
// ---------------------------------------------------------------------------
m2w.options.add([
    { name: 'modelprobe', file: 'custom_draw.cpp',
      doc: 'draw N instances of a .gr2 model on the login screen, pose and skin every frame (1..200)' },
    { name: 'probefile', file: 'custom_draw.cpp',
      doc: 'the model file for ?modelprobe instead of warrior_novice.gr2' },
    { name: 'probehash', file: 'custom_draw.cpp',
      doc: 'with ?modelprobe: print the FNV-1a hash of the canvas after the F-th probe frame (tools/gates/check_probe.py)' },
    { name: 'textureprobe', file: 'custom_draw.cpp',
      doc: 'load every image of a directory through CResourceManager and time it; 1 = d:/ymir work/pc/warrior/' }
]);
