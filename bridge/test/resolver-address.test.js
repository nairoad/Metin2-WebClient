'use strict';

// The address form of a request (`/to/<host>:<port>`) - what our WebAssembly
// client sends. It must reach exactly the declared targets and
// nothing else.

const test = require('node:test');
const assert = require('node:assert/strict');
const { createTargetResolver, UnknownTargetError } = require('../src/target-resolver.js');

const CONFIG = {
  targets: {
    auth: { host: '127.0.0.1', port: 11002 },
    game: { host: '10.0.0.5', port: 13000 },
    named: { host: 'Game.Example.com', port: 13001 },
  },
  defaultTarget: 'auth',
};

test('a declared host:port in the path resolves to its target', () => {
  const resolver = createTargetResolver(CONFIG);
  assert.deepEqual(resolver.resolveRequest('/to/10.0.0.5:13000'),
    { name: 'game', host: '10.0.0.5', port: 13000 });
  assert.equal(resolver.resolveRequest('/to/127.0.0.1:11002').name, 'auth');
});

test('the host is compared without letter case', () => {
  const resolver = createTargetResolver(CONFIG);
  assert.equal(resolver.resolveRequest('/to/game.example.COM:13001').name, 'named');
});

test('an undeclared port of a declared host is refused', () => {
  const resolver = createTargetResolver(CONFIG);
  assert.throws(() => resolver.resolveRequest('/to/10.0.0.5:22'), UnknownTargetError);
});

test('an undeclared host is refused', () => {
  const resolver = createTargetResolver(CONFIG);
  assert.throws(() => resolver.resolveRequest('/to/evil.example.com:13000'), UnknownTargetError);
  assert.throws(() => resolver.resolveRequest('/to/169.254.169.254:80'), UnknownTargetError);
});

test('a malformed address path is refused, not treated as the default target', () => {
  const resolver = createTargetResolver(CONFIG);
  for (const url of ['/to/', '/to/10.0.0.5', '/to/10.0.0.5:', '/to/10.0.0.5:13000/extra',
                     '/to/10.0.0.5:abc', '/to/%E0%A4%A',
                     // normalised away by URL parsing before the fix:
                     '//to/10.0.0.5:13000', '/TO/10.0.0.5:13000', '/To/10.0.0.5:13000',
                     '/./to/10.0.0.5:13000', '/x/../to/10.0.0.5:13000', '/other?target=game',
                     '/to/10.0.0.5%2F..%2F:13000']) {
    assert.throws(() => resolver.resolveRequest(url), UnknownTargetError, url);
  }
});

test('an encoded path is decoded before matching', () => {
  const resolver = createTargetResolver(CONFIG);
  assert.equal(resolver.resolveRequest('/to/10.0.0.5%3A13000').name, 'game');
});

test('the name form still works next to the address form', () => {
  const resolver = createTargetResolver(CONFIG);
  assert.equal(resolver.resolveRequest('/?target=game').name, 'game');
  assert.equal(resolver.resolveRequest('/').name, 'auth');
});

test('an IPv6 host in brackets matches a declared bare IPv6 host', () => {
  const resolver = createTargetResolver({ targets: { v6: { host: '::1', port: 11002 } }, defaultTarget: null });
  assert.equal(resolver.resolveRequest('/to/[::1]:11002').name, 'v6');
});
