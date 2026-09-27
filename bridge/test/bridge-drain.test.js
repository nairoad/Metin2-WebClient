'use strict';

// Frames that were already parsed keep arriving after ws.pause(); under
// sustained TCP backpressure each of them sees write() === false. One
// 'drain' listener must serve them all (one listener per frame
// piled up before).

const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');

const { createBridge } = require('../src/bridge.js');
const { startFakeTcpServer } = require('./helpers/fake-tcp-server.js');
const { waitFor } = require('./helpers/wait.js');

// The smallest stand-in for the ws WebSocket this test needs.
class FakeWebSocket extends EventEmitter {
  constructor() {
    super();
    this.OPEN = 1;
    this.readyState = 1;
    this.pauses = 0;
    this.resumes = 0;
  }
  send() {}
  close() { this.readyState = 3; }
  pause() { this.pauses += 1; }
  resume() { this.resumes += 1; }
}

test('a frame arriving during the pre-connect flush goes after the queued bytes', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();
  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: { tcpConnectTimeoutMs: 5000, highWaterMarkBytes: 1024 * 1024 },
  });
  const written = [];
  let first = true;
  bridge.socket.write = (chunk) => {                    // the first write fills the buffer
    written.push(chunk[0]);
    const ok = !first;
    first = false;
    return ok;
  };
  try {
    ws.emit('message', Buffer.from([0xa1]), true);    // queued: not connected yet
    ws.emit('message', Buffer.from([0xa2]), true);
    await waitFor(() => written.length === 1);         // connect -> flush writes A, waits for drain
    ws.emit('message', Buffer.from([0xa3]), true);    // arrives while B is still queued
    bridge.socket.emit('drain');
    await waitFor(() => written.length === 3);
    assert.deepEqual(written, [0xa1, 0xa2, 0xa3]);
  } finally {
    bridge.close();
    await upstream.close();
  }
});

test('many frames under backpressure register one drain listener and resume once', async () => {
  const upstream = await startFakeTcpServer();
  const ws = new FakeWebSocket();
  const bridge = createBridge({
    ws,
    target: { name: 'auth', host: upstream.host, port: upstream.port },
    config: { tcpConnectTimeoutMs: 5000, highWaterMarkBytes: 1024 * 1024 },
  });
  await waitFor(() => upstream.connections.length === 1);

  try {
    const before = bridge.socket.listenerCount('drain');
    bridge.socket.write = () => false;          // the kernel buffer stays full
    for (let i = 0; i < 5; i += 1) ws.emit('message', Buffer.alloc(16, i), true);

    assert.equal(bridge.socket.listenerCount('drain') - before, 1);
    assert.equal(ws.pauses, 1);

    bridge.socket.emit('drain');
    assert.equal(ws.resumes, 1);
    assert.equal(bridge.socket.listenerCount('drain'), before);
  } finally {
    bridge.close();
    await upstream.close();
  }
});
