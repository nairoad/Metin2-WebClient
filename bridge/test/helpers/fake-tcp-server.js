'use strict';

const net = require('node:net');

// A TCP server standing in for the game server. It records every byte it
// receives and hands back the socket so tests can push bytes downstream.
async function startFakeTcpServer({ onConnection } = {}) {
  const connections = [];

  const server = net.createServer((socket) => {
    const record = {
      socket,
      received: [],
      get bytes() {
        return Buffer.concat(this.received);
      },
    };
    socket.on('data', (chunk) => record.received.push(chunk));
    socket.on('error', () => {});
    connections.push(record);
    if (onConnection) onConnection(record);
  });

  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));

  return {
    host: '127.0.0.1',
    port: server.address().port,
    connections,
    close: () =>
      new Promise((resolve) => {
        for (const record of connections) record.socket.destroy();
        server.close(resolve);
      }),
  };
}

module.exports = { startFakeTcpServer };
