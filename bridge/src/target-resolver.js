'use strict';

class UnknownTargetError extends Error {
  constructor(requested) {
    super(`target "${requested}" is not in the allowlist`);
    this.name = 'UnknownTargetError';
    this.requested = requested;
  }
}

// `/to/<host>:<port>` - the path emscripten's socket layer puts into the
// WebSocket URL when the game connects to `<host>:<port>` (measured from the
// client). An IPv6 host comes in brackets.
const ADDRESS_PATH = /^\/to\/(\[[^\]]+\]|[^/:]+):(\d{1,5})\/?$/;

/// The allowlist lookup for one configuration: resolve (by name), resolveAddress (by host:port), resolveRequest (from the request URL).
function createTargetResolver(config) {
  const { targets, defaultTarget } = config;

  /// A target chosen by NAME (`?target=<name>`), or the default one.
  function resolve(requested) {
    const name =
      requested === undefined || requested === null || requested === ''
        ? defaultTarget
        : requested;

    // hasOwnProperty, not `in` or a plain lookup: inherited keys such as
    // "__proto__" or "toString" must not resolve to anything.
    if (
      typeof name !== 'string' ||
      !Object.prototype.hasOwnProperty.call(targets, name)
    ) {
      throw new UnknownTargetError(String(requested ?? ''));
    }

    const { host, port } = targets[name];
    return { name, host, port };
  }

  /// A target chosen by ADDRESS - honoured only when exactly that host:port is
  /// declared in `targets`. The allowlist decides, never the client: an
  /// address that is not listed is refused like an unknown name, so this is
  /// not an open relay.
  function resolveAddress(host, port) {
    const wantedHost = String(host).replace(/^\[|\]$/g, '').toLowerCase();
    for (const name of Object.keys(targets)) {
      const target = targets[name];
      if (target.host.toLowerCase() === wantedHost && target.port === port) {
        return { name, host: target.host, port: target.port };
      }
    }
    throw new UnknownTargetError(`${host}:${port}`);
  }

  /// The target of a WebSocket request URL. Exactly two forms: the path
  /// `/to/<host>:<port>` (by address), or the path `/` with `?target=<name>`
  /// (by name, or the default). Any other path is refused.
  function resolveRequest(url) {
    // The RAW request path, not `new URL(...).pathname`: WHATWG URL parsing
    // normalises the path first, so `//to/h:p` lost its `/to/` and `/TO/h:p`
    // slipped past the address check - both fell through to the default
    // target (reviewer). Only the exact forms are accepted now.
    const raw = String(url);
    const queryAt = raw.indexOf('?');
    const rawPath = queryAt >= 0 ? raw.slice(0, queryAt) : raw;
    let path;
    try {
      path = decodeURIComponent(rawPath);
    } catch {
      throw new UnknownTargetError(rawPath);
    }
    const match = ADDRESS_PATH.exec(path);
    if (match) return resolveAddress(match[1], Number(match[2]));
    if (path !== '/' && path !== '') throw new UnknownTargetError(path);
    const query = new URLSearchParams(queryAt >= 0 ? raw.slice(queryAt + 1) : '');
    return resolve(query.get('target'));
  }

  return { resolve, resolveAddress, resolveRequest };
}

module.exports = { createTargetResolver, UnknownTargetError };
