#!/usr/bin/env node
/**
 * Chapter 1 from pm-pics vfs-basic.e2e.test.ts, driven through pm-vfs.
 * Auth headers are gone. Home without --user still reports auth_required.
 * Range requests use `get --range` instead of an HTTP Range header.
 */
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const root = mkdtempSync(join(tmpdir(), 'pm-vfs-basic-'));
const ch1 = join(root, 'ch1');
const storage = join(root, 'storage');
mkdirSync(ch1, { recursive: true });
mkdirSync(storage, { recursive: true });
writeFileSync(join(ch1, 'vfs-settings.json'), '{}\n');
writeFileSync(join(ch1, 'hello.txt'), 'hello world');
writeFileSync(join(ch1, 'data.json'), '{"key": "value"}');
mkdirSync(join(ch1, 'sub'));
writeFileSync(join(ch1, 'sub', 'inner.txt'), 'inner content');
writeFileSync(join(ch1, '.hidden-file'), 'should be excluded');
mkdirSync(join(ch1, 'secret'));
writeFileSync(join(ch1, 'secret', 'classified.txt'), 'do not show');
writeFileSync(join(ch1, '.gitignore'), 'secret\n');
writeFileSync(join(ch1, 'large.txt'), 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'.repeat(100));
const cfg = join(root, 'vfs.json');
writeFileSync(cfg, JSON.stringify([
  { name: 'root', type: 'fs', path: storage },
  { name: 'ch1', type: 'fs', path: ch1 },
]));

function vfs(args) {
  return run(['--config', cfg, ...args]);
}
function names(r) {
  return (r.json?.entries || []).map((e) => e.name);
}

const mounts = vfs(['mounts']);
assert(mounts.status === 0, 'mounts exits 0');
assert(mounts.json.mounts.some((m) => m.name === 'ch1'), 'ch1 is listed');
assert(mounts.json.mounts.every((m) => m.path === undefined && m.type === undefined), 'mount objects are name only');

const top = vfs(['ls', 'ch1']);
assert(names(top).includes('hello.txt') && names(top).includes('data.json') && names(top).includes('sub'), 'ls root names');
const sub = vfs(['ls', 'ch1', 'sub']);
assert(names(sub).includes('inner.txt'), 'ls subfolder');

const glob = vfs(['ls', 'ch1', '--glob', '*.json']);
assert(glob.status === 0 && names(glob).includes('data.json') && names(glob).every((n) => n.endsWith('.json')), 'glob *.json');

const missingMount = vfs(['ls', 'nonexistent-mount-xyz']);
assert(missingMount.status !== 0 && missingMount.json?.status === 404, 'unknown mount is 404');
const missingDir = vfs(['ls', 'ch1', 'no-such-dir-xyz']);
assert(missingDir.json?.status === 404, 'missing dir is 404');

const read = vfs(['read', 'ch1', 'hello.txt']);
assert(read.status === 0 && read.json?.text === 'hello world', 'read text');
const got = vfs(['get', 'ch1', 'hello.txt']);
assert(got.status === 0 && String(got.json?.mime || '').includes('text/plain') && got.json?.acceptRanges === 'bytes', 'get text/plain');
const json = vfs(['get', 'ch1', 'data.json']);
assert(String(json.json?.mime || '').includes('application/json'), 'json mime');
const missingFile = vfs(['get', 'ch1', 'no-such-file.txt']);
assert(missingFile.json?.status === 404, 'missing file is 404');
const dirGet = vfs(['get', 'ch1', 'sub']);
assert(dirGet.json?.status === 400, 'get on a directory is 400');

const st = vfs(['stat', 'ch1', 'hello.txt']);
assert(st.status === 0 && st.json?.name === 'hello.txt' && st.json.size > 0 && st.json.mtime !== undefined, 'stat file');

assert(!names(top).includes('vfs-settings.json'), 'vfs-settings.json is hidden');
assert(!names(top).includes('secret'), 'gitignore secret/ is hidden');
assert(!names(top).some((n) => n.startsWith('.')), 'dotfiles are hidden');
const star = vfs(['ls', 'ch1', '--glob', '*']);
assert(!names(star).includes('.gitignore') && !names(star).includes('.hidden-file') && !names(star).includes('vfs-settings.json'), 'glob * still hides defaults');
const denied = vfs(['read', 'ch1', 'vfs-settings.json']);
assert(denied.json?.status === 403, 'reading vfs-settings.json is 403');

const part = vfs(['get', 'ch1', 'large.txt', '--range', 'bytes=10-19']);
assert(part.json?.status === 206 && part.json?.contentRange === 'bytes 10-19/3600' && part.json?.contentLength === 10 && part.json?.body === 'KLMNOPQRST', `range 10-19 (got ${part.stdout})`);
const suffix = vfs(['get', 'ch1', 'large.txt', '--range', 'bytes=-10']);
assert(suffix.json?.status === 206 && suffix.json?.contentLength === 10, 'suffix range');
const oob = vfs(['get', 'ch1', 'large.txt', '--range', 'bytes=5000-6000']);
assert(oob.json?.status === 416 && oob.json?.contentRange === 'bytes */3600', `416 range (got ${oob.stdout})`);

const home = vfs(['ls', 'home']);
assert(home.json?.status === 401 && home.json?.code === 'auth_required', 'home without a user is 401');

rmSync(root, { recursive: true, force: true });
finish(checks);
