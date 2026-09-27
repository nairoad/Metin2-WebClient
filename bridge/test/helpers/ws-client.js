'use strict';

const WebSocket = require('ws');

// Opens a binary WebSocket and records everything the proxy sends back.
async function connectWsClient(url, wsOptions) {
  const ws = new WebSocket(url, wsOptions);
  ws.binaryType = 'nodebuffer';

  const received = [];
  ws.on('message', (data) => received.push(Buffer.from(data)));

  let resolveClosed;
  const closed = new Promise((resolve) => {
    resolveClosed = resolve;
  });
  ws.on('close', (code, reason) => resolveClosed({ code, reason: reason.toString() }));
  ws.on('error', () => {});

  await new Promise((resolve, reject) => {
    ws.once('open', resolve);
    ws.once('close', (code) => reject(new Error(`closed before open: ${code}`)));
    ws.once('error', reject);
  });

  return {
    ws,
    received,
    get bytes() {
      return Buffer.concat(received);
    },
    closed,
  };
}

module.exports = { connectWsClient };
