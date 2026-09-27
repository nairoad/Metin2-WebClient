#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""site_server.py - serves the built client on 127.0.0.1, and ONLY the client.

    python tools/site_server.py [port]        (default 8731)

`python -m http.server` in build/port served everything under it - also
build/port/private (the server address, the bridge configuration, the
account for autologin), the staged sources and the object files (found
in a security audit). This server answers only the files the page needs - the
client itself, its scripts, the measuring page and the corpus - and 404 for
everything else; it binds to 127.0.0.1 only. `webclient.py serve` and the
browser gates use it.
"""
import functools
import http.server
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import workspace                                            # noqa: E402

# Files at the top of build/port a browser may ask for, and the one directory.
ALLOWED_FILES = {'client.html', 'client.js', 'client.wasm', 'client.data', 'sw.js', 'preload.js',
                 'client_measure.html', 'favicon.ico'}
ALLOWED_DIRECTORY = 'corpus'


def allowed(url_path):
    """True when a request path (without the query) names an allowed file:
    one of ALLOWED_FILES at the top, or a plain file name under corpus/."""
    parts = [p for p in url_path.split('/') if p]
    if len(parts) == 1:
        return parts[0] in ALLOWED_FILES
    return (len(parts) == 2 and parts[0] == ALLOWED_DIRECTORY
            and parts[1] not in ('.', '..') and '\\' not in parts[1])


class SiteHandler(http.server.SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler limited to the files `allowed()` accepts."""

    def send_head(self):
        """404 for anything outside the allowed set, then the usual file answer."""
        path = self.path.split('?', 1)[0].split('#', 1)[0]
        from urllib.parse import unquote
        if not allowed(unquote(path)):
            self.send_error(404, 'not part of the client')
            return None
        # a link (symlink/junction) inside corpus/ must not lead out of the
        # build directory
        real = os.path.realpath(self.translate_path(self.path))
        if os.path.commonpath([real, os.path.realpath(workspace.PORT)]) != os.path.realpath(workspace.PORT):
            self.send_error(404, 'not part of the client')
            return None
        return super().send_head()

    def log_request(self, code='-', size='-'):
        """Quiet: only refused and failed requests are printed."""
        if str(getattr(code, 'value', code)).startswith(('4', '5')):
            super().log_request(code, size)


def main(argv):
    """Serves workspace.PORT on 127.0.0.1:<port> until interrupted."""
    port = int(argv[0]) if argv else 8731
    SiteHandler.extensions_map = dict(http.server.SimpleHTTPRequestHandler.extensions_map,
                                      **{'.wasm': 'application/wasm', '.js': 'text/javascript'})
    handler = functools.partial(SiteHandler, directory=workspace.PORT)
    with http.server.ThreadingHTTPServer(('127.0.0.1', port), handler) as server:
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
