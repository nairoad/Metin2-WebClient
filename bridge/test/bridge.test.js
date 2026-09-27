'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');

const { createBridge } = require('../src/bridge.js');
const { CLOSE_CODES } = require('../src/close-codes.js');
const { startFakeTcpServer } = require('./helpers/fake-tcp-server.js');
const { waitFor } = require('./helpers/wait.js');

const CONFIG = {
  tcpConnectTimeoutMs: 5000,
  highWaterMarkBytes: 1024 * 1024,
};

// Minimal stand-in for the ws library's WebSocket, recording what was sent.
class FakeWebSocket extends EventEmitter {
  constructor() {
    super();
    this.OPEN = 1;
    this.readyState = 1;
    this.sent = [];
    this.bufferedAmount = 0;
    this.closed = null;
  }
  send(data, _options, callback) {
    this.sent.push(data);
    if (callback) callback();
  }
  close(code, reason) {
    this.closed = { code, reason };
    this.readyState = 3;
  }
  get bytes() {
    return Buffer.concat(this.sent);
  }
}

test('forwards browser bytes to the TCP server byte-for-byte', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();
  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: CONFIG,
  });

  await waitFor(() => upstream.connections.length === 1);

  const payload = Buffer.from([0x00, 0x01, 0x80, 0xfe, 0xff, 0x7f]);
  ws.emit('message', payload, true);

  await waitFor(() => upstream.connections[0].bytes.length === payload.length);
  assert.deepEqual(upstream.connections[0].bytes, payload);

  bridge.close();
  await upstream.close();
});

test('forwards TCP server bytes to the browser byte-for-byte', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();
  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: CONFIG,
  });

  await waitFor(() => upstream.connections.length === 1);

  const payload = Buffer.from([0xff, 0x00, 0x80, 0x41, 0xc3, 0x28]);
  upstream.connections[0].socket.write(payload);

  await waitFor(() => ws.bytes.length === payload.length);
  assert.deepEqual(ws.bytes, payload);

  bridge.close();
  await upstream.close();
});

test('sends every frame as binary', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();
  const seenOptions = [];
  ws.send = function (data, options, callback) {
    seenOptions.push(options);
    this.sent.push(data);
    if (callback) callback();
  };

  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: CONFIG,
  });
  await waitFor(() => upstream.connections.length === 1);
  upstream.connections[0].socket.write(Buffer.from([0x01]));

  await waitFor(() => seenOptions.length === 1);
  assert.equal(seenOptions[0].binary, true);

  bridge.close();
  await upstream.close();
});

test('preserves a large payload across chunk boundaries', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();
  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: CONFIG,
  });

  await waitFor(() => upstream.connections.length === 1);

  const payload = Buffer.alloc(512 * 1024);
  for (let i = 0; i < payload.length; i += 1) payload[i] = i % 256;
  upstream.connections[0].socket.write(payload);

  await waitFor(() => ws.bytes.length === payload.length, { timeoutMs: 5000 });
  assert.deepEqual(ws.bytes, payload);

  bridge.close();
  await upstream.close();
});

test('concatenates fragmented ws messages into the TCP stream in order', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();
  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: CONFIG,
  });

  await waitFor(() => upstream.connections.length === 1);

  // The ws library delivers a fragmented message as an array of Buffers.
  ws.emit('message', [Buffer.from([0xaa]), Buffer.from([0xbb, 0xcc])], true);

  await waitFor(() => upstream.connections[0].bytes.length === 3);
  assert.deepEqual(upstream.connections[0].bytes, Buffer.from([0xaa, 0xbb, 0xcc]));

  bridge.close();
  await upstream.close();
});

test('queues bytes that arrive before the upstream socket connects', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();
  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: CONFIG,
  });

  // Sent synchronously, long before the TCP handshake can have completed.
  const first = Buffer.from([0x01, 0x02, 0x03]);
  const second = Buffer.from([0x04, 0x05]);
  ws.emit('message', first, true);
  ws.emit('message', second, true);

  await waitFor(() => upstream.connections.length === 1);
  await waitFor(() => upstream.connections[0].bytes.length === 5);
  assert.deepEqual(upstream.connections[0].bytes, Buffer.from([1, 2, 3, 4, 5]));

  bridge.close();
  await upstream.close();
});

test('closes the websocket when the upstream connection is refused', async () => {
  const upstream = await startFakeTcpServer();
  const deadPort = upstream.port;
  await upstream.close(); // nothing is listening on deadPort now

  const ws = new FakeWebSocket();
  createBridge({
    ws,
    target: { name: 'auth', host: '127.0.0.1', port: deadPort },
    config: CONFIG,
  });

  await waitFor(() => ws.closed !== null, { timeoutMs: 5000 });
  assert.equal(ws.closed.code, CLOSE_CODES.UPSTREAM_CONNECT_FAILED);
});

test('closes the websocket when the upstream server disconnects', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();
  createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: CONFIG,
  });

  await waitFor(() => upstream.connections.length === 1);
  upstream.connections[0].socket.end();

  await waitFor(() => ws.closed !== null);
  assert.equal(ws.closed.code, CLOSE_CODES.UPSTREAM_CLOSED);
  await upstream.close();
});

test('destroys the upstream socket when the browser disconnects', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();
  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: CONFIG,
  });

  await waitFor(() => upstream.connections.length === 1);
  ws.readyState = 3;
  ws.emit('close');

  await waitFor(() => bridge.socket.destroyed);
  assert.equal(bridge.socket.destroyed, true);
  await upstream.close();
});

test('gives up when the upstream connection times out', async () => {
  const ws = new FakeWebSocket();
  createBridge({
    ws,
    // 203.0.113.0/24 is TEST-NET-3: reserved for documentation, never routed.
    target: { name: 'auth', host: '203.0.113.1', port: 9999 },
    config: { ...CONFIG, tcpConnectTimeoutMs: 300 },
  });

  await waitFor(() => ws.closed !== null, { timeoutMs: 4000 });
  assert.equal(ws.closed.code, CLOSE_CODES.UPSTREAM_CONNECT_FAILED);
});

test('pauses the upstream socket when the browser falls behind', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();

  // Never invoke the send callback and keep bufferedAmount above the mark:
  // this models a browser that is not draining.
  ws.send = function (data) {
    this.sent.push(data);
    this.bufferedAmount = 10 * 1024 * 1024;
  };

  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: { ...CONFIG, highWaterMarkBytes: 1024 },
  });

  await waitFor(() => upstream.connections.length === 1);
  upstream.connections[0].socket.write(Buffer.alloc(4096, 0xab));

  await waitFor(() => bridge.socket.isPaused());
  assert.equal(bridge.socket.isPaused(), true);

  bridge.close();
  await upstream.close();
});

test('resumes the upstream socket once the browser drains', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();

  let drain = null;
  ws.send = function (data, _options, callback) {
    this.sent.push(data);
    this.bufferedAmount = 10 * 1024 * 1024;
    drain = () => {
      this.bufferedAmount = 0;
      if (callback) callback();
    };
  };

  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: { ...CONFIG, highWaterMarkBytes: 1024 },
  });

  await waitFor(() => upstream.connections.length === 1);
  upstream.connections[0].socket.write(Buffer.alloc(4096, 0xab));

  await waitFor(() => bridge.socket.isPaused());
  await waitFor(() => drain !== null);
  drain();

  await waitFor(() => !bridge.socket.isPaused());
  assert.equal(bridge.socket.isPaused(), false);

  bridge.close();
  await upstream.close();
});

test('pauses the websocket when the pre-connect queue exceeds highWaterMarkBytes', async () => {
  const ws = new FakeWebSocket();
  let paused = false;
  ws.pause = () => {
    paused = true;
  };
  ws.resume = () => {
    paused = false;
  };

  const bridge = createBridge({
    ws,
    // 203.0.113.0/24 is TEST-NET-3: reserved for documentation, never routed,
    // so the TCP handshake never completes and the pre-connect window stays
    // open for the duration of this test.
    target: { name: 'auth', host: '203.0.113.1', port: 9999 },
    config: { tcpConnectTimeoutMs: 60000, highWaterMarkBytes: 1024 },
  });

  assert.equal(paused, false);
  ws.emit('message', Buffer.alloc(2048, 0x01), true);

  assert.equal(paused, true);

  bridge.close();
});

test('pauses the websocket when the upstream socket applies backpressure', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();
  let paused = false;
  ws.pause = () => {
    paused = true;
  };
  ws.resume = () => {
    paused = false;
  };

  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: CONFIG,
  });

  await waitFor(() => upstream.connections.length === 1);

  // Force write() to report a full buffer exactly once.
  // Pure fake — does not call the real socket.write(). Overriding write()'s
  // return value alone does not put a real net.Socket into a needs-drain
  // state (verified empirically), so 'drain' is emitted manually instead.
  let forced = false;
  bridge.socket.write = (chunk) => {
    if (!forced) {
      forced = true;
      setImmediate(() => bridge.socket.emit('drain'));
      return false;
    }
    return true;
  };

  ws.emit('message', Buffer.alloc(64, 0x01), true);
  assert.equal(paused, true);

  await waitFor(() => paused === false, { timeoutMs: 3000 });
  assert.equal(paused, false);

  bridge.close();
  await upstream.close();
});
