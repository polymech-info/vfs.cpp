import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { createAssert, exePath, finish, PKG, requireExe } from './commons.mjs';

const checks = requireExe(createAssert());
const { assert } = checks;

const child = spawn(exePath(), [
  '--config', 'config/vfs-test.json',
  '--binds', 'config/vfs-binds-test.json',
  'serve', '--host', '127.0.0.1', '--port', '0',
], { cwd: PKG, stdio: ['ignore', 'pipe', 'pipe'] });

let stderr = '';
child.stderr.on('data', (chunk) => { stderr += chunk; });
const rl = createInterface({ input: child.stdout });

function firstLine() {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`serve did not print a port\n${stderr}`)), 8000);
    const fail = (err) => { clearTimeout(timer); reject(err); };
    rl.once('line', (line) => { clearTimeout(timer); resolve(line); });
    child.once('exit', (code) => fail(new Error(`serve exited ${code}\n${stderr}`)));
    child.once('error', fail);
  });
}

async function waitTask(base, id, stop = ['completed', 'failed', 'cancelled']) {
  let last = { state: 'missing' };
  for (let i = 0; i < 80; i++) {
    const res = await fetch(`${base}/api/vfs/tasks/${id}`);
    last = await res.json();
    if (stop.includes(last.state)) return last;
    await new Promise((r) => setTimeout(r, 50));
  }
  return last;
}

const stamp = Date.now().toString(36);
const src = `http-src-${stamp}.txt`;
const copied = `http-copy-${stamp}.txt`;
const dst = `http-dst-${stamp}.txt`;
const streamed = `http-stream-${stamp}.txt`;
const names = [src, copied, dst, streamed];
let base = '';

async function cleanup() {
  if (!base) return;
  await Promise.all(names.map((name) =>
    fetch(`${base}/api/vfs/delete/ch1/${name}`, { method: 'DELETE' }).catch(() => {})));
}

async function main() {
  const hello = JSON.parse(await firstLine());
  assert(hello.ok === true && hello.port > 0, `serve prints bound port (${hello.port})`);
  base = `http://${hello.host}:${hello.port}`;

  const mountsRes = await fetch(`${base}/api/vfs/mounts`);
  const mounts = await mountsRes.json();
  assert(mountsRes.status === 200 && mounts.some((m) => m.name === 'ch1'), 'GET mounts');

  const lsRes = await fetch(`${base}/api/vfs/ls/ch1`);
  const listed = await lsRes.json();
  assert(lsRes.status === 200 && listed.some((e) => e.name === 'hello.txt'), 'GET ls ch1');

  const missingRes = await fetch(`${base}/api/vfs/read/ch1/no-such-${stamp}.txt`);
  const missing = await missingRes.json();
  assert(missingRes.status === 404 && missing.code === 'ENOENT', 'missing read is 404');

  const putRes = await fetch(`${base}/api/vfs/write/ch1/${src}`, { method: 'PUT', body: 'from-http' });
  const put = await putRes.json();
  assert(putRes.status === 200 && put.success === true && put.size === 9, 'PUT write');

  const readRes = await fetch(`${base}/api/vfs/read/ch1/${src}`);
  assert(readRes.status === 200 && await readRes.text() === 'from-http', 'GET read');

  const gotRes = await fetch(`${base}/api/vfs/get/ch1/${src}`, { headers: { Range: 'bytes=0-3' } });
  const gotText = await gotRes.text();
  assert(gotRes.status === 206 && gotText === 'from', `GET range 206 (${gotRes.status} ${JSON.stringify(gotText)})`);
  assert(String(gotRes.headers.get('content-range') || '').startsWith('bytes '), 'content-range');

  const copyRes = await fetch(`${base}/api/vfs/copy`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      operation: 'copy',
      source: { mount: 'ch1', path: src },
      destination: { mount: 'ch1', path: copied },
      conflictMode: 'auto',
      conflictStrategy: 'overwrite',
    }),
  });
  const copiedJson = await copyRes.json();
  assert(copyRes.status === 200 && copiedJson.success === true && copiedJson.filesCopied === 1, 'POST copy');

  const transferRes = await fetch(`${base}/api/vfs/transfer`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      operation: 'copy',
      source: { mount: 'ch1', path: src },
      destination: { mount: 'ch1', path: copied },
      conflictMode: 'auto',
      conflictStrategy: 'overwrite',
    }),
  });
  const transfer = await transferRes.json();
  assert(transferRes.status === 200 && typeof transfer.taskId === 'string', 'POST transfer');
  const done = await waitTask(base, transfer.taskId);
  assert(done.state === 'completed' && done.filesDone >= 1, `transfer completed (${done.state})`);

  await fetch(`${base}/api/vfs/write/ch1/${dst}`, { method: 'PUT', body: 'keep-me' });
  const manualRes = await fetch(`${base}/api/vfs/transfer`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      operation: 'copy',
      source: { mount: 'ch1', path: src },
      destination: { mount: 'ch1', path: dst },
      conflictMode: 'manual',
      conflictStrategy: 'error',
    }),
  });
  const manual = await manualRes.json();
  const paused = await waitTask(base, manual.taskId, ['paused_for_conflict', 'completed', 'failed', 'cancelled']);
  assert(paused.state === 'paused_for_conflict' && paused.conflict && paused.conflict.suggestedPath,
    `manual transfer pauses (${paused.state})`);
  const resolved = await fetch(`${base}/api/vfs/tasks/${manual.taskId}/resolve`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ decision: 'skip' }),
  });
  assert(resolved.status === 200, 'resolve skip');
  const after = await waitTask(base, manual.taskId);
  assert(after.state === 'completed', `manual transfer completes after skip (${after.state} ${after.error || ''})`);
  const kept = await fetch(`${base}/api/vfs/read/ch1/${dst}`);
  assert(await kept.text() === 'keep-me', 'skip leaves the destination');

  const streamRes = await fetch(`${base}/api/vfs/transfer`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      operation: 'copy',
      source: { mount: 'ch1', path: src },
      destination: { mount: 'ch1', path: streamed },
      conflictMode: 'auto',
      conflictStrategy: 'overwrite',
    }),
  });
  const streamTask = await streamRes.json();
  const stream = await fetch(`${base}/api/vfs/tasks/${streamTask.taskId}/stream`, {
    signal: AbortSignal.timeout(8000),
  });
  const streamText = await stream.text();
  assert(stream.status === 200 && streamText.includes('"state":"completed"'), 'task stream ends completed');
}

try {
  await main();
} catch (err) {
  console.error(err && err.stack ? err.stack : err);
  assert(false, err && err.message ? err.message : String(err));
} finally {
  await cleanup();
  rl.close();
  child.kill();
  await new Promise((resolve) => {
    const timer = setTimeout(resolve, 300);
    child.once('exit', () => { clearTimeout(timer); resolve(); });
  });
}

finish(checks);
