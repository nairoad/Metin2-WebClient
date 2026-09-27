// SPDX-License-Identifier: GPL-2.0-or-later
// runtime_options_test.js - the address options on a real site vs a local page
// (security audit): on a site only the player options may be read -
// a link to the operator's real page with `?bridge=wss://attacker` or
// `?corpus=https://attacker/` must change nothing. Also m2w.safeUrl.
//
//   node compat/tests/runtime_options_test.js      (tools/gates/run_tests.py runs it)
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const SOURCE = fs.readFileSync(path.join(__dirname, '..', 'runtime.js'), 'utf8');
const ATTACK = '?bridge=wss://attacker.example&corpus=https://attacker.example/&modelprobe=5' +
               '&run=0&missing=1&scale=2&lang=pl&return=https://site.example/&fps=30';
let failures = 0;

/// runtime.js loaded in a fresh context whose page address is `href`.
function load(href) {
  const url = new URL(href);
  const context = {
    URL, URLSearchParams, console: { log() {}, warn() {}, error() {} },
    location: { href: url.href, hostname: url.hostname, protocol: url.protocol, search: url.search },
    Module: {},
  };
  context.globalThis = context;
  vm.createContext(context);
  vm.runInContext('var m2w;\n' + SOURCE, context);
  return context.m2w;
}

/// Prints one check as `<name> OK` / `<name> FAIL` (run_tests.py counts them).
function check(name, condition) {
  console.log(name + (condition ? ' OK' : ' FAIL'));
  if (!condition) failures += 1;
}

const SITES = ['https://play.example.com/metin2/client.html', 'http://192.168.1.10/metin2/client.html',
               'https://127.0.0.1.evil.example/', 'https://localhost.evil.example/',
               'https://LOCALHOST.evil.example/', 'http://0.0.0.0/', 'https://[2001:db8::1]/'];
for (const site of SITES) {
  const m2w = load(site + ATTACK);
  const tag = new URL(site).hostname;
  check('site ' + tag + ': not local', m2w.isLocalPage() === false);
  for (const name of ['bridge', 'corpus', 'modelprobe', 'run', 'missing'])
    check('site ' + tag + ': ?' + name + ' ignored', m2w.options.get(name) === null);
  check('site ' + tag + ': ?scale read', m2w.options.get('scale') === '2');
  check('site ' + tag + ': ?lang read', m2w.options.get('lang') === 'pl');
  check('site ' + tag + ': ?return read', m2w.options.get('return') === 'https://site.example/');
}

for (const local of ['http://127.0.0.1:8731/client.html', 'http://localhost:8731/client.html',
                     'http://[::1]:8731/client.html', 'file:///C:/game/client.html']) {
  const m2w = load(local + ATTACK);
  const tag = local.split('/')[2] || 'file';
  check('local ' + tag + ': is local', m2w.isLocalPage() === true);
  check('local ' + tag + ': ?bridge read', m2w.options.get('bridge') === 'wss://attacker.example');
  check('local ' + tag + ': ?corpus read', m2w.options.get('corpus') === 'https://attacker.example/');
}

// every option row: a player option is marked `site`, a technical one is not
{
  const m2w = load('https://play.example.com/');
  const player = new Set(['scale', 'max', 'cursor', 'return', 'lang', 'deflang', 'corpusMB',
                          'noprefetch', 'nofetch', 'fps']);
  for (const row of m2w.options.table)
    check('row ?' + row.name + ' site flag', !!row.site === player.has(row.name));
}

// m2w.safeUrl: only http(s) addresses
{
  const m2w = load('https://play.example.com/metin2/client.html');
  for (const bad of ['javascript:alert(1)', ' JaVaScRiPt:alert(1)', 'java\tscript:x', 'data:text/html,x',
                     'vbscript:x', 'file:///etc/passwd'])
    check('safeUrl refuses ' + JSON.stringify(bad), m2w.safeUrl(bad) === '');
  check('safeUrl keeps https', m2w.safeUrl('https://a.example/x') === 'https://a.example/x');
  check('safeUrl resolves relative', m2w.safeUrl('/home') === 'https://play.example.com/home');
}

process.exit(failures ? 1 : 0);
