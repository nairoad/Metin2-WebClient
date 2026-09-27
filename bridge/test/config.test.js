'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { loadConfig, ConfigError } = require('../src/config.js');

const MINIMAL = { targets: { auth: { host: '127.0.0.1', port: 11002 } } };

test('applies defaults for omitted fields', () => {
  const cfg = loadConfig(MINIMAL);
  assert.equal(cfg.listenHost, '127.0.0.1');
  assert.equal(cfg.listenPort, 9000);
  assert.equal(cfg.maxConnections, 200);
  assert.equal(cfg.tcpConnectTimeoutMs, 5000);
});

test('preserves explicitly provided fields', () => {
  const cfg = loadConfig({ ...MINIMAL, listenPort: 9999, maxConnections: 5 });
  assert.equal(cfg.listenPort, 9999);
  assert.equal(cfg.maxConnections, 5);
});

test('rejects a config with no targets', () => {
  assert.throws(() => loadConfig({ targets: {} }), ConfigError);
});

test('rejects a target with an out-of-range port', () => {
  assert.throws(
    () => loadConfig({ targets: { auth: { host: '127.0.0.1', port: 70000 } } }),
    ConfigError,
  );
});

test('rejects a target with a missing host', () => {
  assert.throws(() => loadConfig({ targets: { auth: { port: 11002 } } }), ConfigError);
});

test('rejects a defaultTarget that is not declared', () => {
  assert.throws(() => loadConfig({ ...MINIMAL, defaultTarget: 'nope' }), ConfigError);
});

test('accepts a defaultTarget that is declared', () => {
  const cfg = loadConfig({ ...MINIMAL, defaultTarget: 'auth' });
  assert.equal(cfg.defaultTarget, 'auth');
});
