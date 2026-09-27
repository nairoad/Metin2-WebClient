'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const WebSocket = require('ws');

const { loadConfig } = require('../src/config.js');
const { createProxyServer, clientAddress } = require('../src/server.js');
const { CLOSE_CODES } = require('../src/close-codes.js');
const { startFakeTcpServer } = require('./helpers/fake-tcp-server.js');
const { connectWsClient } = require('./helpers/ws-client.js');
const { waitFor } = require('./helpers/wait.js');

async function startStack(overrides = {}) {
  const upstream = await startFakeTcpServer();
  const config = loadConfig({
    listenPort: 0,
    targets: { auth: { host: upstream.host, port: upstream.port } },
    defaultTarget: 'auth',
    ...overrides,
  });
  const server = createProxyServer(config, {});
  const { port } = await server.listen();
  return {
    upstream,
    server,
    port,
    url: (query = '') => `ws://127.0.0.1:${port}/${query}`,
    close: async () => {
      await server.close();
      await upstream.close();
    },
  };
}

test('accepts a connection and opens one upstream socket', async () => {
  const stack = await startStack();
  const client = await connectWsClient(stack.url('?target=auth'));

  await waitFor(() => stack.upstream.connections.length === 1);
  assert.equal(stack.upstream.connections.length, 1);
  assert.equal(stack.server.activeCount(), 1);

  client.ws.close();
  await stack.close();
});

test('uses defaultTarget when no target is given', async () => {
  const stack = await startStack();
  const client = await connectWsClient(stack.url());

  await waitFor(() => stack.upstream.connections.length === 1);
  assert.equal(stack.upstream.connections.length, 1);

  client.ws.close();
  await stack.close();
});

test('rejects an undeclared target without opening a socket', async () => {
  const stack = await startStack();

  const ws = new WebSocket(stack.url('?target=admin'));
  const result = await new Promise((resolve) => {
    ws.on('close', (code) => resolve(code));
    ws.on('error', () => {});
  });

  assert.equal(result, CLOSE_CODES.UNKNOWN_TARGET);
  assert.equal(stack.upstream.connections.length, 0);
  await stack.close();
});

test('refuses connections beyond maxConnections', async () => {
  const stack = await startStack({ maxConnections: 1 });
  const first = await connectWsClient(stack.url());
  await waitFor(() => stack.server.activeCount() === 1);

  const second = new WebSocket(stack.url());
  const code = await new Promise((resolve) => {
    second.on('close', (c) => resolve(c));
    second.on('error', () => {});
  });

  assert.equal(code, CLOSE_CODES.CONNECTION_LIMIT);

  first.ws.close();
  await stack.close();
});

test('closes an idle connection after idleTimeoutMs', async () => {
  const stack = await startStack({ idleTimeoutMs: 250 });
  const client = await connectWsClient(stack.url());

  const { code } = await client.closed;
  assert.equal(code, CLOSE_CODES.IDLE_TIMEOUT);
  await stack.close();
});

test('a quiet session stays open by default (no idle close)', async () => {
  const stack = await startStack({ heartbeatMs: 100 });
  const client = await connectWsClient(stack.url());
  await waitFor(() => stack.upstream.connections.length === 1);

  await new Promise((r) => setTimeout(r, 600));
  assert.equal(client.ws.readyState, WebSocket.OPEN);

  client.ws.close();
  await stack.close();
});

test('the heartbeat closes a session whose browser stopped answering pings', async () => {
  const stack = await startStack({ heartbeatMs: 100 });
  const client = await connectWsClient(stack.url(), { autoPong: false });
  await waitFor(() => stack.upstream.connections.length === 1);

  const { code } = await client.closed;
  assert.equal(code, 1006);            // terminated: no close handshake
  await waitFor(() => stack.server.activeCount() === 0);
  await stack.close();
});

test('maxConnectionsPerAddress refuses one address over the limit, not another', async () => {
  const stack = await startStack({ maxConnectionsPerAddress: 1 });
  const a1 = await connectWsClient(stack.url(), { headers: { 'X-Forwarded-For': '198.51.100.7' } });
  await waitFor(() => stack.upstream.connections.length === 1);
  // the same player address again: refused
  const a2 = new WebSocket(stack.url(), { headers: { 'X-Forwarded-For': '198.51.100.7' } });
  const refused = await new Promise((resolve) => a2.on('close', (code) => resolve(code)));
  assert.equal(refused, CLOSE_CODES.CONNECTION_LIMIT);
  // another player address: accepted
  const b1 = await connectWsClient(stack.url(), { headers: { 'X-Forwarded-For': '198.51.100.8' } });
  await waitFor(() => stack.upstream.connections.length === 2);
  assert.equal(b1.ws.readyState, WebSocket.OPEN);
  a1.ws.close();
  b1.ws.close();
  await stack.close();
});

test('X-Forwarded-For is ignored when trustProxy is off', async () => {
  const stack = await startStack({ maxConnectionsPerAddress: 1, trustProxy: false });
  const a1 = await connectWsClient(stack.url(), { headers: { 'X-Forwarded-For': '198.51.100.7' } });
  await waitFor(() => stack.upstream.connections.length === 1);
  // a different header, but the real peer is the same machine: refused
  const a2 = new WebSocket(stack.url(), { headers: { 'X-Forwarded-For': '198.51.100.9' } });
  const refused = await new Promise((resolve) => a2.on('close', (code) => resolve(code)));
  assert.equal(refused, CLOSE_CODES.CONNECTION_LIMIT);
  a1.ws.close();
  await stack.close();
});

test('clientAddress trusts X-Forwarded-For only from this machine', () => {
  const req = (peer, xff) => ({ socket: { remoteAddress: peer }, headers: xff ? { 'x-forwarded-for': xff } : {} });
  const on = { trustProxy: true };
  // from nginx on this machine: the last address nginx appended
  assert.equal(clientAddress(req('127.0.0.1', '10.9.9.9, 198.51.100.7'), on), '198.51.100.7');
  assert.equal(clientAddress(req('::1', '198.51.100.7'), on), '198.51.100.7');
  assert.equal(clientAddress(req('::ffff:127.0.0.1', '198.51.100.7'), on), '198.51.100.7');
  // from anyone else: the header is ignored, the peer counts
  assert.equal(clientAddress(req('203.0.113.5', '198.51.100.7'), on), '203.0.113.5');
  assert.equal(clientAddress(req('::ffff:203.0.113.5', '127.0.0.1'), on), '::ffff:203.0.113.5');
  // trustProxy off, or no header: the peer
  assert.equal(clientAddress(req('127.0.0.1', '198.51.100.7'), { trustProxy: false }), '127.0.0.1');
  assert.equal(clientAddress(req('127.0.0.1', ''), on), '127.0.0.1');
});

test('traffic resets the idle timer', async () => {
  const stack = await startStack({ idleTimeoutMs: 400 });
  const client = await connectWsClient(stack.url());
  await waitFor(() => stack.upstream.connections.length === 1);

  for (let i = 0; i < 4; i += 1) {
    await new Promise((r) => setTimeout(r, 150));
    client.ws.send(Buffer.from([i]), { binary: true });
  }

  assert.equal(client.ws.readyState, WebSocket.OPEN);

  client.ws.close();
  await stack.close();
});

test('rejects a websocket handshake whose Origin is not in allowedOrigins', async () => {
  const stack = await startStack({ allowedOrigins: ['https://allowed.example'] });

  const ws = new WebSocket(stack.url(), {
    headers: { Origin: 'https://evil.example' },
  });
  const result = await new Promise((resolve) => {
    ws.on('unexpected-response', (_req, res) => resolve({ status: res.statusCode }));
    ws.on('close', (code) => resolve({ code }));
    ws.on('error', () => {});
  });

  assert.ok(result.status !== undefined || result.code !== undefined);
  assert.equal(stack.upstream.connections.length, 0);
  await stack.close();
});

test('accepts a websocket handshake whose Origin is in allowedOrigins', async () => {
  const stack = await startStack({ allowedOrigins: ['https://allowed.example'] });

  const client = await connectWsClient(stack.url(), {
    headers: { Origin: 'https://allowed.example' },
  });
  await waitFor(() => stack.upstream.connections.length === 1);
  assert.equal(stack.upstream.connections.length, 1);

  client.ws.close();
  await stack.close();
});

test('accepts any origin when allowedOrigins is unset (default behavior)', async () => {
  const stack = await startStack();

  const client = await connectWsClient(stack.url(), {
    headers: { Origin: 'https://anything.example' },
  });
  await waitFor(() => stack.upstream.connections.length === 1);
  assert.equal(stack.upstream.connections.length, 1);

  client.ws.close();
  await stack.close();
});

test('activeCount returns to zero after clients disconnect', async () => {
  const stack = await startStack();
  const client = await connectWsClient(stack.url());
  await waitFor(() => stack.server.activeCount() === 1);

  client.ws.close();
  await waitFor(() => stack.server.activeCount() === 0);
  assert.equal(stack.server.activeCount(), 0);

  await stack.close();
});
