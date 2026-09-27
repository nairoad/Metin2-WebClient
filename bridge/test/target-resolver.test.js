'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { createTargetResolver, UnknownTargetError } = require('../src/target-resolver.js');

const CONFIG = {
  targets: {
    auth: { host: '127.0.0.1', port: 11002 },
    game: { host: '10.0.0.5', port: 13000 },
  },
  defaultTarget: 'auth',
};

test('resolves a declared target to its host and port', () => {
  const resolver = createTargetResolver(CONFIG);
  assert.deepEqual(resolver.resolve('game'), { name: 'game', host: '10.0.0.5', port: 13000 });
});

test('falls back to defaultTarget when the request is empty', () => {
  const resolver = createTargetResolver(CONFIG);
  assert.equal(resolver.resolve('').name, 'auth');
  assert.equal(resolver.resolve(null).name, 'auth');
  assert.equal(resolver.resolve(undefined).name, 'auth');
});

test('rejects an undeclared target name', () => {
  const resolver = createTargetResolver(CONFIG);
  assert.throws(() => resolver.resolve('admin'), UnknownTargetError);
});

test('refuses a raw host:port supplied by the client', () => {
  const resolver = createTargetResolver(CONFIG);
  assert.throws(() => resolver.resolve('evil.example.com:22'), UnknownTargetError);
  assert.throws(() => resolver.resolve('127.0.0.1:22'), UnknownTargetError);
});

test('is not fooled by inherited Object properties', () => {
  const resolver = createTargetResolver(CONFIG);
  assert.throws(() => resolver.resolve('__proto__'), UnknownTargetError);
  assert.throws(() => resolver.resolve('constructor'), UnknownTargetError);
  assert.throws(() => resolver.resolve('toString'), UnknownTargetError);
});

test('rejects everything when defaultTarget is null and nothing is requested', () => {
  const resolver = createTargetResolver({ targets: CONFIG.targets, defaultTarget: null });
  assert.throws(() => resolver.resolve(''), UnknownTargetError);
});

test('the error carries the rejected name for logging', () => {
  const resolver = createTargetResolver(CONFIG);
  try {
    resolver.resolve('admin');
    assert.fail('expected UnknownTargetError');
  } catch (err) {
    assert.equal(err.requested, 'admin');
  }
});
