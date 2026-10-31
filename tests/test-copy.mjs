#!/usr/bin/env node
/**
 * Local copy/move cases from two suites:
 *   pm-pics server/src/products/storage/__tests__/vfs-copy.e2e.test.ts
 *   pixlwiz tests/orchestrator/test-vfs-copy.mjs
 *
 * Remote hops (ftp/sftp/ssh), shell globs, ${CWD}, dry-run, and the Windows
 * share-lock / cross-volume cases stay on tanit-cli (`npm run test:vfs-copy`).
 * Background transfer tasks are not in this CLI yet.
 */
import { mkdtempSync, mkdirSync, readFileSync, rmSync, utimesSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const root = mkdtempSync(join(tmpdir(), 'pm-vfs-copy-'));
const srcDir = join(root, 'src');
const dstDir = join(root, 'dst');
mkdirSync(srcDir);
mkdirSync(dstDir);
const cfg = join(root, 'vfs.json');
writeFileSync(cfg, JSON.stringify([
  { name: 'ch3copy', type: 'fs', path: srcDir },
  { name: 'ch3copydst', type: 'fs', path: dstDir },
]));
const req = join(root, 'request.json');

function write(dir, rel, text) {
  const abs = join(dir, ...rel.split('/'));
  mkdirSync(join(abs, '..'), { recursive: true });
  writeFileSync(abs, text);
}

write(srcDir, 'copy-src.txt', 'copy source content');
write(srcDir, 'nested/child/a.txt', 'A');
write(srcDir, 'nested/child/b.txt', 'B');
write(srcDir, 'nested/node_modules/pkg.json', '{"name":"skip-me"}');
write(srcDir, 'nested/child/keep.log', 'keep-log');
write(dstDir, 'existing.txt', 'destination old');

function copy(body) {
  writeFileSync(req, JSON.stringify(body));
  return run(['--config', cfg, 'copy', '--request', req]);
}

const one = copy({
  source: { mount: 'ch3copy', path: 'copy-src.txt' },
  destination: { mount: 'ch3copydst', path: 'copied.txt' },
  conflictMode: 'auto',
  conflictStrategy: 'error',
});
assert(one.status === 0 && readFileSync(join(dstDir, 'copied.txt'), 'utf8') === 'copy source content', 'copies a single file');

const tree = copy({
  source: { mount: 'ch3copy', path: 'nested' },
  destination: { mount: 'ch3copydst', path: 'copied-tree' },
  conflictStrategy: 'overwrite',
});
assert(tree.status === 0, 'recursive copy exits 0');
assert(readFileSync(join(dstDir, 'copied-tree', 'child', 'a.txt'), 'utf8') === 'A', 'copied a.txt');
assert(readFileSync(join(dstDir, 'copied-tree', 'child', 'b.txt'), 'utf8') === 'B', 'copied b.txt');

const filtered = copy({
  operation: 'copy',
  source: { mount: 'ch3copy', path: 'nested' },
  destination: { mount: 'ch3copydst', path: 'filtered-tree' },
  conflictStrategy: 'overwrite',
  includePatterns: ['**/*.txt', '**/*.log'],
  excludePatterns: ['**/node_modules/**', '**/*.log'],
  excludeDefault: true,
});
assert(filtered.status === 0, 'filtered copy exits 0');
assert(readFileSync(join(dstDir, 'filtered-tree', 'child', 'a.txt'), 'utf8') === 'A', 'include kept txt');
let skippedLog = false;
let skippedPkg = false;
try { readFileSync(join(dstDir, 'filtered-tree', 'child', 'keep.log')); } catch { skippedLog = true; }
try { readFileSync(join(dstDir, 'filtered-tree', 'node_modules', 'pkg.json')); } catch { skippedPkg = true; }
assert(skippedLog && skippedPkg, 'exclude dropped log and node_modules');

write(srcDir, 'batch/a1.txt', 'A1');
write(srcDir, 'batch/a2.txt', 'A2');
const batch = copy({
  sources: [
    { mount: 'ch3copy', path: 'batch/a1.txt' },
    { mount: 'ch3copy', path: 'batch/a2.txt' },
  ],
  destination: { mount: 'ch3copydst', path: 'batch-dst' },
  conflictStrategy: 'overwrite',
});
assert(batch.status === 0, 'multi-source copy exits 0');
assert(readFileSync(join(dstDir, 'batch-dst', 'a1.txt'), 'utf8') === 'A1', 'batch a1');
assert(readFileSync(join(dstDir, 'batch-dst', 'a2.txt'), 'utf8') === 'A2', 'batch a2');

write(srcDir, 'existing.txt', 'source new');
const conflict = copy({
  source: { mount: 'ch3copy', path: 'existing.txt' },
  destination: { mount: 'ch3copydst', path: 'existing.txt' },
  conflictStrategy: 'error',
});
assert(conflict.status !== 0 && conflict.json?.status === 409, 'strategy=error is 409');
assert(readFileSync(join(dstDir, 'existing.txt'), 'utf8') === 'destination old', 'error left dest unchanged');

const skipped = copy({
  source: { mount: 'ch3copy', path: 'existing.txt' },
  destination: { mount: 'ch3copydst', path: 'existing.txt' },
  conflictStrategy: 'skip',
});
assert(skipped.status === 0 && readFileSync(join(dstDir, 'existing.txt'), 'utf8') === 'destination old', 'strategy=skip');

const renamed = copy({
  source: { mount: 'ch3copy', path: 'existing.txt' },
  destination: { mount: 'ch3copydst', path: 'existing.txt' },
  conflictStrategy: 'rename',
});
assert(renamed.status === 0, 'strategy=rename exits 0');
assert(readFileSync(join(dstDir, 'existing (copy 1).txt'), 'utf8') === 'source new', 'rename wrote existing (copy 1).txt');

const manual = copy({
  source: { mount: 'ch3copy', path: 'existing.txt' },
  destination: { mount: 'ch3copydst', path: 'existing.txt' },
  conflictMode: 'manual',
  conflictStrategy: 'overwrite',
});
assert(manual.json?.status === 409 && String(manual.json?.conflict?.suggestedPath || '').includes('ch3copydst:'), 'manual conflict suggests a path');

write(srcDir, 'if-newer.txt', 'SRC-OLD');
write(dstDir, 'if-newer.txt', 'DST-NEWER');
const now = Date.now();
utimesSync(join(srcDir, 'if-newer.txt'), new Date(now - 60_000), new Date(now - 60_000));
utimesSync(join(dstDir, 'if-newer.txt'), new Date(now), new Date(now));
const older = copy({
  source: { mount: 'ch3copy', path: 'if-newer.txt' },
  destination: { mount: 'ch3copydst', path: 'if-newer.txt' },
  conflictStrategy: 'if_newer',
});
assert(older.status === 0 && readFileSync(join(dstDir, 'if-newer.txt'), 'utf8') === 'DST-NEWER', 'if_newer skips an older source');

write(srcDir, 'if-newer.txt', 'SRC-NEWER');
utimesSync(join(srcDir, 'if-newer.txt'), new Date(now + 60_000), new Date(now + 60_000));
const newer = copy({
  source: { mount: 'ch3copy', path: 'if-newer.txt' },
  destination: { mount: 'ch3copydst', path: 'if-newer.txt' },
  conflictStrategy: 'if_newer',
});
assert(newer.status === 0 && readFileSync(join(dstDir, 'if-newer.txt'), 'utf8') === 'SRC-NEWER', 'if_newer overwrites when the source is newer');

const overwrite = copy({
  source: { mount: 'ch3copy', path: 'existing.txt' },
  destination: { mount: 'ch3copydst', path: 'copied.txt' },
  conflictStrategy: 'overwrite',
});
assert(overwrite.status === 0 && readFileSync(join(dstDir, 'copied.txt'), 'utf8') === 'source new', 'overwrite replaces dest');

write(srcDir, 'move-me.txt', 'relocate');
const moved = copy({
  operation: 'move',
  source: { mount: 'ch3copy', path: 'move-me.txt' },
  destination: { mount: 'ch3copydst', path: 'moved.txt' },
  conflictStrategy: 'error',
});
assert(moved.status === 0 && readFileSync(join(dstDir, 'moved.txt'), 'utf8') === 'relocate', 'move landed the file');
let sourceGone = false;
try { readFileSync(join(srcDir, 'move-me.txt')); } catch { sourceGone = true; }
assert(sourceGone, 'move removed the source file');

write(srcDir, 'stay.txt', 'leave-me');
write(dstDir, 'stay.txt', 'keep');
const moveSkip = copy({
  operation: 'move',
  source: { mount: 'ch3copy', path: 'stay.txt' },
  destination: { mount: 'ch3copydst', path: 'stay.txt' },
  conflictStrategy: 'skip',
});
assert(moveSkip.status === 0 && readFileSync(join(dstDir, 'stay.txt'), 'utf8') === 'keep', 'move --skip left dest');
assert(readFileSync(join(srcDir, 'stay.txt'), 'utf8') === 'leave-me', 'move --skip left source');

rmSync(root, { recursive: true, force: true });
finish(checks);
