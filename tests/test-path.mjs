#!/usr/bin/env node
/**
 * Port of the pm-pics path-sanitizer cases. The jail symlink case is the
 * local half of the TS resolvePath check.
 */
import { mkdtempSync, mkdirSync, symlinkSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

function check(input, { ok, path, status } = {}) {
  const r = run(['check-path', '--input', input]);
  if (ok) {
    assert(r.status === 0 && r.json?.path === path, `allow ${JSON.stringify(input)} -> ${path} (got ${r.stdout})`);
  } else {
    assert(r.status !== 0 && r.json?.status === (status || 403), `reject ${JSON.stringify(input)} (got ${r.status} ${r.stdout})`);
  }
}

check('', { ok: true, path: '' });
check('a/b', { ok: true, path: 'a/b' });
check('/a/b', { ok: true, path: 'a/b' });
check('a//b', { ok: true, path: 'a/b' });
check('a/./b', { ok: true, path: 'a/b' });
check('.', { ok: true, path: '' });
check('..', { status: 403 });
check('foo/../../etc', { status: 403 });
check('%2e%2e/%2e%2e', { status: 403 });
check('%252e%252e', { status: 403 });
check('~/secret', { status: 403 });
check('C:/Windows', { ok: true, path: 'C/Windows' });

const written = run(['check-path', '--write', '--input', 'My Dir/a<b>.txt']);
assert(written.status === 0 && written.json?.path === 'My Dir/ab.txt', `write path strips illegal characters (got ${written.json?.path})`);

const dir = run(['check-path', '--write', '--dir', '--input', 'My.Dir']);
assert(dir.status === 0 && dir.json?.path === 'My.Dir', 'directory write keeps dots');

const root = mkdtempSync(join(tmpdir(), 'pm-vfs-jail-'));
const outside = join(root, 'outside.txt');
const jail = join(root, 'jail');
mkdirSync(jail);
writeFileSync(outside, 'secret');
writeFileSync(join(root, 'vfs.json'), JSON.stringify([{ name: 'box', type: 'fs', path: jail }]));
let linked = false;
try {
  symlinkSync(outside, join(jail, 'escape.txt'));
  linked = true;
} catch {
  console.log('  skip  symlink (privilege)');
}
if (linked) {
  const escaped = run(['--config', join(root, 'vfs.json'), 'read', 'box', 'escape.txt']);
  assert(escaped.status !== 0 && escaped.json?.status === 403, `symlink escape is forbidden (got ${escaped.stdout} ${escaped.stderr})`);
}
rmSync(root, { recursive: true, force: true });

finish(checks);
