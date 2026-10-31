#!/usr/bin/env node
/**
 * Shipped config/fixtures, altered from pm-pics server/config/vfs-test.json
 * and vfs-binds-test.json. Machine-absolute mounts are fixture directories
 * inside this package.
 */
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { PKG, createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const cfg = join(PKG, 'config', 'vfs-test.json');
const binds = join(PKG, 'config', 'vfs-binds-test.json');
const names = JSON.parse(readFileSync(cfg, 'utf8')).map((m) => m.name);

const listed = run(['--config', cfg, '--binds', binds, 'mounts']);
assert(listed.status === 0, 'mounts exits 0');
const got = (listed.json?.mounts || []).map((m) => m.name);
for (const name of names) assert(got.includes(name), `mount ${name} is listed`);
assert(listed.json.mounts.every((m) => m.path === undefined && m.type === undefined), 'mounts do not leak path or type');

const hello = run(['--config', cfg, '--binds', binds, 'ls', 'ch1']);
assert(hello.status === 0, 'ls ch1 exits 0');
assert((hello.json?.entries || []).some((e) => e.name === 'hello.txt'), 'fixture hello.txt is listed');
assert(!(hello.json?.entries || []).some((e) => e.name.startsWith('.')), 'dotfiles stay hidden on the fixture mount');

const root = run(['--config', cfg, '--binds', binds, 'ls', 'root']);
assert(root.status === 0, 'ls root exits 0');
assert((root.json?.entries || []).some((e) => e.name === 'assets' && e.type === 'dir'), 'root-assets bind shows up as a virtual directory');

const marker = run(['--config', cfg, '--binds', binds, 'read', 'root', 'assets/marker.txt']);
assert(marker.status === 0 && marker.json?.text === 'marker', `read through root/assets bind (got ${marker.stdout})`);

finish(checks);
