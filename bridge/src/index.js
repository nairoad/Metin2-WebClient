'use strict';

const fs = require('node:fs');
const path = require('node:path');

const { loadConfig, ConfigError } = require('./config.js');
const { createProxyServer } = require('./server.js');

/// Writes one event as a JSON line to stdout (with a timestamp).
function jsonLogger(event, details = {}) {
  process.stdout.write(
    `${JSON.stringify({ ts: new Date().toISOString(), event, ...details })}\n`,
  );
}

/// The config file: the first argument, else PROXY_CONFIG, else config.local.json, else config.default.json.
function resolveConfigPath() {
  const fromArgv = process.argv[2];
  if (fromArgv) return path.resolve(fromArgv);
  if (process.env.PROXY_CONFIG) return path.resolve(process.env.PROXY_CONFIG);

  const local = path.join(__dirname, '..', 'config.local.json');
  if (fs.existsSync(local)) return local;
  return path.join(__dirname, '..', 'config.default.json');
}

/// Loads the configuration, starts the bridge and stops it cleanly on SIGINT/SIGTERM.
async function main() {
  const configPath = resolveConfigPath();
  let config;
  try {
    config = loadConfig(JSON.parse(fs.readFileSync(configPath, 'utf8')));
  } catch (err) {
    const detail = err instanceof ConfigError ? err.message : String(err);
    process.stderr.write(`failed to load config from ${configPath}: ${detail}\n`);
    process.exit(1);
  }

  const server = createProxyServer(config, { logger: jsonLogger });
  const { port } = await server.listen();
  jsonLogger('listening', {
    host: config.listenHost,
    port,
    configPath,
    targets: Object.keys(config.targets).length,   // a count: the list can run to hundreds
  });

  let shuttingDown = false;
  /// Closes every session and the server once, then exits.
  const shutdown = async (signal) => {
    if (shuttingDown) return;
    shuttingDown = true;
    jsonLogger('shutdown', { signal, active: server.activeCount() });
    await server.close();
    process.exit(0);
  };

  process.on('SIGINT', () => shutdown('SIGINT'));
  process.on('SIGTERM', () => shutdown('SIGTERM'));
}

main().catch((err) => {
  process.stderr.write(`fatal: ${err.stack}\n`);
  process.exit(1);
});
