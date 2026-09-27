'use strict';

// End to end: a WebSocket opened with our client's URL form
// (`ws://bridge/to/<host>:<port>`, subprotocol `binary`) reaches the declared
// TCP target, and an undeclared address is closed with UNKNOWN_TARGET before
// any TCP connection is made.

const test = require('node:test');
const assert = require('node:assert/strict');

const { loadConfig } = require('../src/config.js');
const { createProxyServer } = require('../src/server.js');
const { CLOSE_CODES } = require('../src/close-codes.js');
const { startFakeTcpServer } = require('./helpers/fake-tcp-server.js');
const { connectWsClient } = require('./helpers/ws-client.js');
const { waitFor } = require('./helpers/wait.js');

async function startStack() {
  const upstream = await startFakeTcpServer();
  const config = loadConfig({
    listenPort: 0,
    targets: { auth: { host: upstream.host, port: upstream.port } },
    defaultTarget: null,
  });
  const server = createProxyServer(config, {});
  const { port } = await server.listen();
  return {
    upstream,
    base: `ws://127.0.0.1:${port}`,
    close: async () => {
      await server.close();
      await upstream.close();
    },
  };
}

test('a declared address in the path is bridged, with the binary subprotocol', async () => {
  const stack = await startStack();
  const client = await connectWsClient(`${stack.base}/to/${stack.upstream.host}:${stack.upstream.port}`, ['binary']);
  assert.equal(client.ws.protocol, 'binary');

  await waitFor(() => stack.upstream.connections.length === 1);
  client.ws.send(Buffer.from([0x01, 0xff, 0x80]));
  await waitFor(() => stack.upstream.connections[0].bytes.length === 3);
  assert.deepEqual([...stack.upstream.connections[0].bytes], [0x01, 0xff, 0x80]);

  client.ws.close();
  await stack.close();
});

test('an undeclared address is refused and no TCP connection is made', async () => {
  const stack = await startStack();
  const client = await connectWsClient(`${stack.base}/to/${stack.upstream.host}:1`, ['binary']);
  const { code } = await client.closed;
  assert.equal(code, CLOSE_CODES.UNKNOWN_TARGET);
  assert.equal(stack.upstream.connections.length, 0);
  await stack.close();
});
