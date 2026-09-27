'use strict';

const net = require('node:net');
const { CLOSE_CODES } = require('./close-codes.js');

// Normalises whatever the ws library hands us into a single Buffer.
// A fragmented message arrives as an array of Buffers.
/// One Buffer from what ws delivers (a Buffer, or Buffer[] for a fragmented message); throws on anything else.
function toBuffer(data) {
  if (Buffer.isBuffer(data)) return data;
  if (Array.isArray(data)) return Buffer.concat(data);
  // The ws library always delivers a Buffer or Buffer[] for a correctly
  // configured server. Anything else means something upstream is
  // misconfigured (e.g. a text frame slipping through) — Buffer.from(string)
  // would lossily re-encode as UTF-8 and corrupt bytes >= 0x80, so fail
  // loudly instead of silently corrupting the stream.
  throw new Error('unexpected websocket message type: ' + typeof data);
}

/// Joins one browser WebSocket to one TCP connection to `target`, byte for byte, with backpressure both ways; returns { socket, close }.
function createBridge({ ws, target, config, logger = () => {} }) {
  const socket = net.connect({ host: target.host, port: target.port });
  socket.setNoDelay(true);
  // A quiet game connection is normal (a player standing still); TCP
  // keepalive notices a game server that vanished without closing.
  socket.setKeepAlive(true, 60000);

  // Bytes the browser sends before the TCP handshake completes. The client's
  // first packet regularly lands here, so it must be buffered, never dropped.
  let pending = [];
  let pendingBytes = 0;
  let connected = false;
  let finished = false;
  let upstreamPaused = false;
  // True whenever we have called ws.pause() ourselves (either because the
  // pre-connect queue grew past highWaterMarkBytes, or because a flush write
  // hit backpressure on the TCP socket). Tracked with a single flag since ws
  // is either paused by us or it isn't.
  let wsPaused = false;

  const connectTimer = setTimeout(() => {
    logger('upstream_connect_timeout', { target: target.name });
    finish(CLOSE_CODES.UPSTREAM_CONNECT_FAILED, 'upstream connect timeout');
  }, config.tcpConnectTimeoutMs);

  /// Ends the session once: stops the connect timer, drops the queue, destroys the TCP socket and closes the WebSocket with `code`.
  function finish(code, reason) {
    if (finished) return;
    finished = true;
    clearTimeout(connectTimer);
    pending = [];
    pendingBytes = 0;
    socket.destroy();
    if (ws.readyState === ws.OPEN) ws.close(code, reason);
  }

  socket.on('connect', () => {
    clearTimeout(connectTimer);
    connected = true;
    logger('upstream_connected', { target: target.name });
    flushPending();
  });

  // Drains the pre-connect queue onto the now-open TCP socket, applying the
  // same WS -> TCP backpressure the steady-state message handler uses: if
  // socket.write() reports a full kernel buffer, stop and wait for 'drain'
  // before writing the rest of the queue.
  /// Writes the bytes queued before the TCP connect, pausing the WebSocket while the socket is full.
  function flushPending() {
    while (pending.length > 0) {
      const chunk = pending.shift();
      pendingBytes -= chunk.length;
      const ok = socket.write(chunk);
      if (!ok) {
        if (!wsPaused && ws.readyState === ws.OPEN) {
          wsPaused = true;
          ws.pause();
        }
        socket.once('drain', flushPending);
        return;
      }
    }
    pendingBytes = 0;
    if (wsPaused) {
      wsPaused = false;
      if (ws.readyState === ws.OPEN) ws.resume();
    }
  }

  // TCP -> WS backpressure. The ws library has no 'drain' event, so the
  // send callback plus bufferedAmount is what tells us the browser caught up.
  socket.on('data', (chunk) => {
    if (ws.readyState !== ws.OPEN) return;

    ws.send(chunk, { binary: true }, () => {
      if (upstreamPaused && ws.bufferedAmount < config.highWaterMarkBytes) {
        upstreamPaused = false;
        socket.resume();
      }
    });

    if (!upstreamPaused && ws.bufferedAmount > config.highWaterMarkBytes) {
      upstreamPaused = true;
      socket.pause();
    }
  });

  socket.on('error', (err) => {
    logger('upstream_error', { target: target.name, message: err.message });
    finish(
      connected ? CLOSE_CODES.UPSTREAM_CLOSED : CLOSE_CODES.UPSTREAM_CONNECT_FAILED,
      connected ? 'upstream error' : 'upstream connect failed',
    );
  });

  // No half-open: the game protocol has no half-close semantics, so an
  // upstream FIN ends the session in both directions.
  socket.on('close', () => {
    finish(
      connected ? CLOSE_CODES.UPSTREAM_CLOSED : CLOSE_CODES.UPSTREAM_CONNECT_FAILED,
      connected ? 'upstream closed' : 'upstream connect failed',
    );
  });

  ws.on('message', (data) => {
    const chunk = toBuffer(data);

    // Until the pre-connect queue is fully written, new frames go to its END:
    // writing one directly while flushPending waits for 'drain' would put it
    // on the wire before older bytes (reviewer - found in the
    // original, the game protocol has no way to recover from reordering).
    if (!connected || pending.length > 0) {
      pending.push(chunk);
      pendingBytes += chunk.length;
      // Bound the pre-connect queue: without this, a slow/never-completing
      // TCP handshake (up to tcpConnectTimeoutMs) lets a client push
      // unlimited data into memory. Pause the websocket until the queue is
      // flushed on connect.
      if (!wsPaused && pendingBytes > config.highWaterMarkBytes) {
        wsPaused = true;
        ws.pause();
      }
      return;
    }

    // WS -> TCP backpressure: stop reading frames until the kernel buffer drains.
    // Frames already parsed can still arrive after ws.pause(), each with a full
    // write(); ONE drain listener is enough - registering one per frame piles
    // up listeners (and MaxListeners warnings) under sustained backpressure.
    if (!socket.write(chunk) && !wsPaused) {
      wsPaused = true;
      ws.pause();
      socket.once('drain', () => {
        wsPaused = false;
        if (ws.readyState === ws.OPEN) ws.resume();
      });
    }
  });

  ws.on('close', () => {
    logger('client_closed', { target: target.name });
    finish();
  });

  ws.on('error', (err) => {
    logger('client_error', { target: target.name, message: err.message });
    finish();
  });

  return {
    socket,
    close(code, reason) {
      finish(code, reason);
    },
  };
}

module.exports = { createBridge };
