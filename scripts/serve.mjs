import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { exePath, PKG } from '../tests/commons.mjs';

const exe = exePath();
if (!existsSync(exe)) {
  console.error(`pm-vfs not built (${exe}). From packages/vfs.cpp run: npm run build:cpp`);
  process.exit(1);
}

const host = process.env.PM_VFS_HOST || '127.0.0.1';
const port = process.env.PM_VFS_PORT || '8787';
const extra = process.argv.slice(2);
const args = [
  '--config', 'config/vfs-test.json',
  '--binds', 'config/vfs-binds-test.json',
  'serve',
];
if (!extra.some((arg) => arg === '--host' || arg.startsWith('--host='))) args.push('--host', host);
if (!extra.some((arg) => arg === '--port' || arg.startsWith('--port='))) args.push('--port', port);
args.push(...extra);

const child = spawn(exe, args, { cwd: PKG, stdio: 'inherit' });

child.on('exit', (code, signal) => {
  process.exit(signal ? 1 : (code ?? 1));
});
