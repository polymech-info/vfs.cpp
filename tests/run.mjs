#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const dir = dirname(fileURLToPath(import.meta.url));
const files = [
  'test-path.mjs',
  'test-config.mjs',
  'test-basic.mjs',
  'test-copy.mjs',
  'test-binds.mjs',
  'test-quota.mjs',
  'test-http.mjs',
];

let failed = 0;
for (const file of files) {
  console.log(`\n== ${file}`);
  const r = spawnSync(process.execPath, [join(dir, file)], { stdio: 'inherit' });
  if ((r.status ?? 1) !== 0) failed++;
}
process.exit(failed ? 1 : 0);
