# site/ - the page around the game

| file | what it is |
|---|---|
| `client.html` | The page players open. `tools/link_client.py` fills in the build version and, if you set one, the bridge address. |
| `preload.js` | The download bar in the corner and the background download of the rest of the game data. |
| `sw.js` | The Service Worker: keeps the downloaded data chunks in the browser, so the next start is fast (on HTTPS and on localhost; browsers allow it nowhere else). |
| `measure_shell.html` | The measuring page the quality gates run the client in (frame counter, log). Not uploaded to your site. |

The game itself (`client.js`, `client.wasm`, `client.data`) is built by
`python webclient.py build`; `python webclient.py package` puts it together
with these files into `dist/site/`. How to put that on a web server:
[docs/DEPLOYMENT.md](../docs/DEPLOYMENT.md).
