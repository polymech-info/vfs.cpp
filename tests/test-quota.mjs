#!/usr/bin/env node
/**
 * Quota predicates from vfs-quota.test.ts, plus one real home write.
 * Cached admin roles stay a host callback, so that mock is not ported.
 */
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const fallback = run(['quota'], { env: { VFS_QUOTA_DEFAULT_MB: '' } });
assert(fallback.json?.defaultMb === 100 && fallback.json?.defaultBytes === 100 * 1024 * 1024 && fallback.json?.unlimited === false, 'default quota is 100 MiB');

const unlimited = run(['quota'], { env: { VFS_QUOTA_DEFAULT_MB: '0' } });
assert(unlimited.json?.unlimited === true && unlimited.json?.defaultBytes === 0, '0 MB is unlimited');
const noCap = run(['quota', '--used', '1000000000000', '--incoming', '1000000000000'], { env: { VFS_QUOTA_DEFAULT_MB: '0' } });
assert(noCap.json?.exceed === false, 'unlimited does not exceed');

const over = run(['quota', '--used', '90', '--incoming', '20', '--limit', '100']);
const fit = run(['quota', '--used', '90', '--incoming', '20', '--limit', '100', '--replacing', '15']);
assert(over.json?.exceed === true, '20 bytes over a 100 byte cap exceeds');
assert(fit.json?.exceed === false, 'replacing 15 bytes fits');

const home = run(['quota', '--home-target', '--mount', 'home', '--owner', 'u1']);
const bind = run(['quota', '--home-target', '--mount', 'home', '--owner', 'u1', '--bind']);
const other = run(['quota', '--home-target', '--mount', 'assets', '--owner', 'u1']);
const anon = run(['quota', '--home-target', '--mount', 'home']);
assert(home.json?.homeTarget === true, 'home with a user is a quota target');
assert(bind.json?.homeTarget === false && other.json?.homeTarget === false && anon.json?.homeTarget === false, 'binds, other mounts, and anonymous are not');

const adminId = '3bb4cfbf-318b-44d3-a9d3-35680e738421';
const admin = run(['quota', '--admin-id', adminId], { env: { DEFAULT_ADMIN_ID: `"${adminId}"` } });
const notAdmin = run(['quota', '--admin-id', '00000000-0000-0000-0000-000000000000'], { env: { DEFAULT_ADMIN_ID: adminId } });
assert(admin.json?.admin === true && notAdmin.json?.admin === false, 'quoted DEFAULT_ADMIN_ID matches');

const mapped = run(['errno', 'EDQUOT']);
assert(mapped.json?.status === 507 && mapped.json?.error === 'Quota exceeded' && mapped.json?.code === 'EDQUOT', 'EDQUOT maps to 507');
const enospc = run(['errno', 'ENOSPC']);
assert(enospc.json?.status === 507 && enospc.json?.error === 'Insufficient Storage', 'ENOSPC maps to 507');
const canon = run(['errno', 'EFORBIDDEN', '--canonical']);
assert(canon.json?.code === 'EACCES', 'EFORBIDDEN canonicalizes to EACCES');
const guessed = run(['errno', '--guess', 'EDQUOT: quota exceeded']);
assert(guessed.json?.code === 'EDQUOT', 'guess errno from a message');
const table = [
  ['ENOENT', 404], ['ENOTDIR', 404], ['EACCES', 403], ['EISDIR', 400], ['EINVAL', 400],
  ['EEXIST', 409], ['EFBIG', 413], ['EIO', 500],
];
for (const [code, status] of table) {
  const row = run(['errno', code]);
  assert(row.json?.status === status, `${code} -> ${status}`);
}

const dir = mkdtempSync(join(tmpdir(), 'pm-vfs-quota-'));
const storage = join(dir, 'storage');
const assets = join(dir, 'assets');
mkdirSync(storage);
mkdirSync(assets);
writeFileSync(join(dir, 'vfs.json'), JSON.stringify([
  { name: 'root', type: 'fs', path: storage },
  { name: 'assets', type: 'fs', path: assets },
]));
writeFileSync(join(dir, 'binds.json'), JSON.stringify([
  { id: 'home-assets', source: { mount: 'assets' }, target: { mount: 'home', path: 'assets' } },
]));
const oneByte = String(1 / 1024 / 1024);
const env = { VFS_QUOTA_DEFAULT_MB: oneByte };
const base = ['--config', join(dir, 'vfs.json'), '--binds', join(dir, 'binds.json'), '--user', 'user-1'];
const blocked = run([...base, 'write', 'home', 'big.txt', '--data', '0123456789'], { env });
assert(blocked.json?.status === 507 && blocked.json?.code === 'EDQUOT', `home write over the cap is EDQUOT (got ${blocked.stdout})`);
const via = run([...base, 'write', 'home', 'assets/via.txt', '--data', '0123456789'], { env });
assert(via.status === 0, `bind write does not count against home (got ${via.stdout} ${via.stderr})`);

rmSync(dir, { recursive: true, force: true });
finish(checks);
