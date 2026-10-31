#!/usr/bin/env node
/**
 * Bind cases from vfs-write-binds and vfs-nested, without ACL.
 * A readonly bind rejects writes. A cycle returns ELOOP.
 */
import { existsSync, mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const root = mkdtempSync(join(tmpdir(), 'pm-vfs-bind-'));
const rootDir = join(root, 'root');
const assets = join(root, 'assets');
const deep = join(root, 'deep');
const shared = join(root, 'shared');
const mountA = join(root, 'a');
const mountB = join(root, 'b');
mkdirSync(join(rootDir, 'real-dir', 'level1'), { recursive: true });
mkdirSync(assets);
mkdirSync(deep);
mkdirSync(shared);
mkdirSync(mountA);
mkdirSync(mountB);
writeFileSync(join(rootDir, 'real-dir', 'level1', 'file.txt'), 'real');
writeFileSync(join(deep, 'marker.txt'), 'deep-marker');
writeFileSync(join(shared, 'a.txt'), 'shared-a');
writeFileSync(join(assets, 'already.txt'), 'assets');

const cfg = join(root, 'vfs.json');
const binds = join(root, 'binds.json');
writeFileSync(cfg, JSON.stringify([
  { name: 'root', type: 'fs', path: rootDir },
  { name: 'assets', type: 'fs', path: assets },
  { name: 'deepbind', type: 'fs', path: deep },
  { name: 'ch2-shared', type: 'fs', path: shared },
  { name: 'A', type: 'fs', path: mountA },
  { name: 'B', type: 'fs', path: mountB },
]));
writeFileSync(binds, JSON.stringify([
  { id: 'root-assets', source: { mount: 'assets' }, target: { mount: 'root', path: 'assets' }, acl: 'source' },
  { id: 'root-deep', source: { mount: 'deepbind' }, target: { mount: 'root', path: 'real-dir/level1/level2' }, acl: 'source' },
  { id: 'root-shared', source: { mount: 'ch2-shared' }, target: { mount: 'root', path: 'shared' }, readonly: true },
  { id: 'a-to-b', source: { mount: 'B' }, target: { mount: 'A', path: 'to-b' } },
  { id: 'b-to-a', source: { mount: 'A' }, target: { mount: 'B', path: 'to-a' } },
]));

function vfs(args) {
  return run(['--config', cfg, '--binds', binds, ...args]);
}

const listed = vfs(['ls', 'root']);
assert((listed.json?.entries || []).some((e) => e.name === 'assets' && e.type === 'dir'), 'virtual assets directory');
assert((listed.json?.entries || []).some((e) => e.name === 'real-dir'), 'real directory still listed');

const level = vfs(['ls', 'root', 'real-dir/level1']);
const levelNames = (level.json?.entries || []).map((e) => e.name);
assert(levelNames.includes('file.txt') && levelNames.includes('level2'), `deep bind is visible beside real files (${levelNames.join(',')})`);

const marker = vfs(['read', 'root', 'real-dir/level1/level2/marker.txt']);
assert(marker.status === 0 && marker.json?.text === 'deep-marker', 'read through the deep bind');

const wrote = vfs(['write', 'root', 'assets/bind-written.txt', '--data', 'via-bind']);
assert(wrote.status === 0, `write through bind (got ${wrote.stdout} ${wrote.stderr})`);
assert(existsSync(join(assets, 'bind-written.txt')), 'file landed on the source mount');
assert(readFileSync(join(assets, 'bind-written.txt'), 'utf8') === 'via-bind', 'source bytes match');

const sharedRead = vfs(['read', 'root', 'shared/a.txt']);
assert(sharedRead.status === 0 && sharedRead.json?.text === 'shared-a', 'readonly bind can be read');
const sharedWrite = vfs(['write', 'root', 'shared/nope.txt', '--data', 'x']);
assert(sharedWrite.status !== 0 && sharedWrite.json?.status === 403, 'readonly bind rejects writes');

const cycle = vfs(['read', 'A', 'to-b/to-a/to-b/x.txt']);
assert(cycle.status !== 0 && cycle.json?.code === 'ELOOP', `bind cycle is ELOOP (got ${cycle.stdout})`);

rmSync(root, { recursive: true, force: true });
finish(checks);
