'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');

const { loadConfig } = require('../src/config.js');
const { createProxyServer } = require('../src/server.js');
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
    url: `ws://127.0.0.1:${port}/?target=auth`,
    close: async () => {
      await server.close();
      await upstream.close();
    },
  };
}

test('every byte value 0x00-0xFF survives the round trip to the server', async () => {
  const stack = await startStack();
  const client = await connectWsClient(stack.url);
  await waitFor(() => stack.upstream.connections.length === 1);

  const payload = Buffer.from(Array.from({ length: 256 }, (_, i) => i));
  client.ws.send(payload, { binary: true });

  await waitFor(() => stack.upstream.connections[0].bytes.length === 256);
  assert.deepEqual(stack.upstream.connections[0].bytes, payload);

  client.ws.close();
  await stack.close();
});

test('every byte value 0x00-0xFF survives the round trip to the browser', async () => {
  const stack = await startStack();
  const client = await connectWsClient(stack.url);
  await waitFor(() => stack.upstream.connections.length === 1);

  const payload = Buffer.from(Array.from({ length: 256 }, (_, i) => 255 - i));
  stack.upstream.connections[0].socket.write(payload);

  await waitFor(() => client.bytes.length === 256);
  assert.deepEqual(client.bytes, payload);

  client.ws.close();
  await stack.close();
});

test('a 4 MB random stream arrives intact and in order', async () => {
  const stack = await startStack();
  const client = await connectWsClient(stack.url);
  await waitFor(() => stack.upstream.connections.length === 1);

  const payload = crypto.randomBytes(4 * 1024 * 1024);
  stack.upstream.connections[0].socket.write(payload);

  await waitFor(() => client.bytes.length === payload.length, { timeoutMs: 20000 });
  assert.equal(
    crypto.createHash('sha256').update(client.bytes).digest('hex'),
    crypto.createHash('sha256').update(payload).digest('hex'),
  );

  client.ws.close();
  await stack.close();
});

test('many small writes preserve ordering exactly', async () => {
  const stack = await startStack();
  const client = await connectWsClient(stack.url);
  await waitFor(() => stack.upstream.connections.length === 1);

  const expected = Buffer.alloc(2000);
  for (let i = 0; i < 1000; i += 1) {
    const chunk = Buffer.from([i & 0xff, (i >> 8) & 0xff]);
    chunk.copy(expected, i * 2);
    client.ws.send(chunk, { binary: true });
  }

  await waitFor(() => stack.upstream.connections[0].bytes.length === 2000, {
    timeoutMs: 10000,
  });
  assert.deepEqual(stack.upstream.connections[0].bytes, expected);

  client.ws.close();
  await stack.close();
});

test('concurrent sessions never cross streams', async () => {
  const stack = await startStack();
  const a = await connectWsClient(stack.url);
  const b = await connectWsClient(stack.url);
  await waitFor(() => stack.upstream.connections.length === 2);

  a.ws.send(Buffer.from('AAAA'), { binary: true });
  b.ws.send(Buffer.from('BBBB'), { binary: true });

  await waitFor(
    () =>
      stack.upstream.connections[0].bytes.length === 4 &&
      stack.upstream.connections[1].bytes.length === 4,
  );

  const seen = [
    stack.upstream.connections[0].bytes.toString(),
    stack.upstream.connections[1].bytes.toString(),
  ].sort();
  assert.deepEqual(seen, ['AAAA', 'BBBB']);

  // And downstream, each client only hears its own server.
  stack.upstream.connections[0].socket.write(Buffer.from('1111'));
  await waitFor(() => a.bytes.length + b.bytes.length === 4);
  assert.equal(a.bytes.length === 4 ? b.bytes.length : a.bytes.length, 0);

  a.ws.close();
  b.ws.close();
  await stack.close();
});

test('closing one session leaves the other running', async () => {
  const stack = await startStack();
  const a = await connectWsClient(stack.url);
  const b = await connectWsClient(stack.url);
  await waitFor(() => stack.server.activeCount() === 2);

  a.ws.close();
  await waitFor(() => stack.server.activeCount() === 1);

  b.ws.send(Buffer.from([0x42]), { binary: true });
  await waitFor(() =>
    stack.upstream.connections.some((c) => c.bytes.includes(0x42)),
  );

  b.ws.close();
  await stack.close();
});
