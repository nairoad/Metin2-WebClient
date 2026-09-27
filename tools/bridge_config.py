#!/usr/bin/env python3
"""bridge_config.py - the configuration of our WebSocket -> TCP bridge (bridge/).

A browser cannot open TCP, so the client connects to the bridge with
`ws://<bridge>/to/<host>:<port>` and the bridge opens TCP to the game server.
The bridge connects ONLY to the addresses listed here (an allowlist - a bridge
that connects anywhere is an open relay). They come from:

  1. the game's own server list - `serverInfo.py` of the built client data
     (build/port/data): every "ip" with its "port" / "tcp_port" in the dict
     literals, names resolved from the module's constants. Read with `ast`,
     never executed;
  2. the PORTS THE SERVER ANNOUNCES AT RUN TIME: after the character is
     chosen, the channel sends the client on to the game core that hosts
     the map - another port of the same host, which no client file lists
     (measured: channel 13000 -> core 13002, refused until then).
     So for every host of the server list the bridge also allows the
     `[bridge] port_span` ports (default 100) from each listed port up:
     11000-11099, 13000-13129 for a list with 11000 and 13000..13030.
     Still only the server's own hosts, never another machine;
  3. `[bridge] targets` in webclient.toml ("host:port" strings) - for
     addresses the list does not show (e.g. a core on another host).

The result goes to build/port/private/bridge.json (it holds the server
address - private, outside git) and is what `python webclient.py serve`
starts the bridge with.

    python tools/bridge_config.py        # write it and print the target count
    python tools/bridge_config.py --site https://play.example.com
                                         # for your web server: only that page may connect,
                                         # written to build/port/private/bridge-site.json;
                                         # repeat --site for every address players open
                                         # (e.g. the domain AND the LAN address)
"""

import ast
import io
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import workspace                                            # noqa: E402

SERVER_INFO = os.path.join(workspace.PORT, 'data', 'serverInfo.py')
OUTPUT = os.path.join(workspace.PRIVATE, 'bridge.json')
BRIDGE_DIR = os.path.join(workspace.ROOT, 'bridge')

# How many ports from each port of the server list up are allowed on the
# same host (the cores the server announces at run time) - see the top.
DEFAULT_PORT_SPAN = 100

# Where the client looks for the bridge on localhost (compat/runtime.js,
# m2w.bridgeStart).
DEFAULT_LISTEN_PORT = 11496


def module_constants(tree):
    """Module-level `NAME = <str or int literal>` assignments of a parsed file."""
    constants = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Constant):
            for target in node.targets:
                if isinstance(target, ast.Name) and isinstance(node.value.value, (str, int)):
                    constants[target.id] = node.value.value
    return constants


def value_of(node, constants):
    """A literal, or a name resolved through `constants`; None otherwise."""
    if isinstance(node, ast.Constant):
        return node.value
    if isinstance(node, ast.Name):
        return constants.get(node.id)
    return None


def server_list_addresses(path=SERVER_INFO):
    """Every (host, port) of the game's server list: each dict literal with an
    "ip" and a "port" or "tcp_port" key. [] when the file is missing."""
    if not os.path.isfile(path):
        return []
    with io.open(path, encoding='utf-8', errors='replace') as f:
        tree = ast.parse(f.read())
    constants = module_constants(tree)
    found = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Dict):
            continue
        entries = {value_of(k, constants): v for k, v in zip(node.keys, node.values) if k is not None}
        host = value_of(entries['ip'], constants) if 'ip' in entries else None
        port_node = entries.get('tcp_port', entries.get('port'))
        port = value_of(port_node, constants) if port_node is not None else None
        if isinstance(host, str) and host and isinstance(port, int) and 0 < port < 65536:
            if (host, port) not in found:
                found.append((host, port))
    return found


def configured_addresses():
    """`[bridge] targets` of webclient.toml as (host, port); exits on a malformed entry."""
    result = []
    for item in workspace.setting('bridge', 'targets', default=[]) or []:
        host, _, port = str(item).rpartition(':')
        if not host or not port.isdigit():
            sys.exit('webclient.toml [bridge] targets: %r is not "host:port"' % item)
        result.append((host.strip('[]'), int(port)))
    return result


def build_config(page_port=8731, sites=None):
    """The bridge configuration dict: listen address, the allowlist, and the
    page origins allowed to connect: the local page on `page_port`, or - for
    a real site - only `sites` (e.g. ['https://play.example.com']); plus
    `[bridge] origins` in both cases."""
    listed = server_list_addresses()
    span = int(workspace.setting('bridge', 'port_span', default=DEFAULT_PORT_SPAN))
    addresses = []
    for host, port in listed:
        for p in range(port, min(port + max(span, 1), 65536)):
            if (host, p) not in addresses:
                addresses.append((host, p))
    for address in configured_addresses():
        if address not in addresses:
            addresses.append(address)
    listen_port = int(workspace.setting('bridge', 'listen', default=DEFAULT_LISTEN_PORT))
    if sites:
        origins = [s.rstrip('/') for s in sites]
    else:
        origins = ['http://127.0.0.1:%d' % page_port, 'http://localhost:%d' % page_port]
    origins += [str(o) for o in (workspace.setting('bridge', 'origins', default=[]) or [])]
    return {
        'listenHost': workspace.setting('bridge', 'host', default='127.0.0.1'),
        'listenPort': listen_port,
        'maxConnectionsPerAddress': int(workspace.setting('bridge', 'max_per_address', default=0)),
        'defaultTarget': None,
        'allowedOrigins': origins,
        'targets': {'t%d' % (i + 1): {'host': h, 'port': p} for i, (h, p) in enumerate(addresses)},
    }


def write_config(page_port=8731):
    """Writes build/port/private/bridge.json; returns (path, config)."""
    config = build_config(page_port)
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with io.open(OUTPUT, 'w', encoding='utf-8') as f:
        json.dump(config, f, indent=2)
    return OUTPUT, config


def main():
    """Writes the configuration (the local one, or with `--site URL` the one for a
    web server) and prints what the bridge will allow (counts only)."""
    sites = [sys.argv[i + 1] if i + 1 < len(sys.argv) else ''
             for i, a in enumerate(sys.argv) if a == '--site']
    for site in sites:
        if not site.startswith(('https://', 'http://')) or site.count('/') > 3 or \
                (site.count('/') == 3 and not site.endswith('/')):
            sys.exit('--site takes the ORIGIN of your game page, e.g. https://play.example.com '
                     '(scheme and host, no path); repeat it for every address players open')
    if sites:
        config = build_config(sites=sites)
        path = os.path.join(workspace.PRIVATE, 'bridge-site.json')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with io.open(path, 'w', encoding='utf-8') as f:
            json.dump(config, f, indent=2)
    else:
        path, config = write_config()
    if not config['targets']:
        print('no targets: build the client data first (serverInfo.py) or set [bridge] targets')
        return 1
    print('%s: %d hosts, ports %s, listening on %s:%d, pages allowed: %s'
          % (os.path.relpath(path, workspace.ROOT),
             len({t['host'] for t in config['targets'].values()}),
             port_ranges(t['port'] for t in config['targets'].values()),
             config['listenHost'], config['listenPort'], ', '.join(config['allowedOrigins'])))
    for site in sites:
        warn_unreachable(site, config)
    return 0


def is_private_host(host):
    """True for a loopback or private-network IP literal (127.x, 10.x,
    172.16-31.x, 192.168.x, fc00::/7) and for localhost; False for a public IP
    or any other name (a domain is assumed to be public)."""
    import ipaddress
    host = host.strip('[]')
    if host == 'localhost' or host.endswith('.localhost'):
        return True
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        return False
    return address.is_private or address.is_loopback


def warn_unreachable(site, config):
    """Warns (without printing the addresses) when the page is on a public
    host but the game server addresses are private-network ones: the bridge
    dials the game server FROM THE WEB SERVER, so a web server outside the
    game server's network cannot reach them - build the client with the
    address the web server can reach (`[server] address` in webclient.toml)."""
    from urllib.parse import urlsplit
    site_host = urlsplit(site).hostname or ''
    private = {t['host'] for t in config['targets'].values() if is_private_host(t['host'])}
    if private and not is_private_host(site_host):
        print('WARNING: %d game server host(s) have a private-network address (192.168.x.x, 10.x.x.x ...)'
              ' and the page is on a public host. That works ONLY when the bridge runs inside the'
              ' game server network. If your web server is elsewhere, set [server] address in'
              ' webclient.toml to an address the web server can reach and build again'
              ' (docs/DEPLOYMENT.md, "When the web server is outside the game server network").'
              % len(private))


def port_ranges(ports):
    """'11000-11099, 13000-13129' - a sorted set of ports as ranges."""
    ports = sorted(set(ports))
    ranges, start = [], None
    for i, p in enumerate(ports):
        if start is None:
            start = p
        if i + 1 == len(ports) or ports[i + 1] != p + 1:
            ranges.append(str(start) if start == p else '%d-%d' % (start, p))
            start = None
    return ', '.join(ranges)


if __name__ == '__main__':
    sys.exit(main())
