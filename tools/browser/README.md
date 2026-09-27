# Measuring the client in a browser, from the command line

Before this harness the work loop looked like this: build, ask a human to
open a tab, ask them to paste the log. One round per message.

These two scripts close that loop in a single command. They drive **Edge**
(it is Chromium, so `puppeteer-core` drives it the same way as Chrome) —
they do not download a browser of their own, because this one is already on disk.

## Setup — once

```sh
cd tools/browser
npm install
```

## Running

```sh
# in a separate shell, from build/port/
python -m http.server 8731 --bind 127.0.0.1

# here
node measure.js "http://127.0.0.1:8731/client.html?run=1&missing=1" 45000
```

Returns: the page log, `syserr.txt`, `missing.txt`, the last error trace
and a screenshot in `zrzut.png`. Window size through `M2W_WIDTH` and `M2W_HEIGHT`.

## When the client stops responding

```sh
node stack.js "http://127.0.0.1:8731/client.html?run=1" 25000
```

Pauses execution and prints the **call stack**, with function names — the client
is linked with `-g2`, so the stack shows
`CPythonApplication::SetCursorVisible`, not `wasm-function[30218]`.

Early on this single call settled a matter the log had no way
to show: the client hung in the loop `do { } while (ShowCursor(FALSE) >= 0)`,
because our `ShowCursor` returned a constant zero. No error, no message —
formally nothing happened.

## Two traps, both paid for

**WebGL in headless mode.** There is no graphics card there, so Chromium
has to draw in software. Without `--use-angle=swiftshader`
and `--enable-unsafe-swiftshader`, `getContext("webgl2")` returns `null`
and the client ends at creating the device.

**Measuring code must not create a canvas.** The first version of
`measure.js` did `createElement("canvas")` and asked for a context — every half
second. The browser has a hard limit of active WebGL contexts and after
exceeding it takes away the **oldest**; the oldest was the client's context.
The tool killed what it measured, and showed a black screen as the result.
