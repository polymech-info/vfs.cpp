# pm-vfs

Server-side virtual filesystem. This is the C++ port of the pm-pics storage product
(`server/src/products/storage`: Hono routes under `/api/vfs`, local jail, binds,
quota). It is a standalone package. The main app links it from source with
`add_subdirectory`. It does not replace `src/vfs`.

`src/vfs` is the desktop **client**: `vfs://` / `ftp://` / `ssh://` / `mtp://`,
the copy queue, and `tanit-cli cp`. `test:vfs-copy` stays on that client.
`packages/vfs.cpp` is the **jail and the API** those clients talk to.

ACL is not part of this package. HTTP is not part of the library. Both sit
outside the core and can be left out of a build.

## Layers

```
pm-vfs  (static lib, no sockets, no ACL, no auth product)
  config        mounts + binds JSON, same shape as the TS loaders
  path          sanitizer + jail (realpath must stay under the mount root)
  local         list, stat, read, write, mkdir, remove
  binds         longest-prefix rewrite, virtual directory entries
  quota         optional byte cap on home writes
  tasks         in-process copy / move / delete with a conflict pause
  json          VfsNode and error bodies

pm-vfs CLI     CLI11 process over the lib. First thing we ship and test.
  subcommands call the lib directly. No listening socket.

HTTP adapters  translate /api/vfs/* onto the lib. Pick one at compile time.
  httplib      default. Already FetchContent'd (cpp-httplib v0.16.3).
  drogon       optional wrapper only. packages/drogon stays source for
               other app servers and is not a vfs dependency.

packages/acl.cpp   optional. Implements IVfsAuthorizer. Default build
                   links AllowAll and never opens vfs-settings.json.
```

A caller is a plain struct: `user_id` (empty means anonymous). The CLI passes
`--user`. An HTTP adapter fills it from whatever auth the host already has.
This package does not link Better Auth, Postgres, or a user directory.

## Why the HTTP server is replaceable

Drogon is a poor fit for the Android build, and the VFS core should not know
which server it is behind.

Drogon is Linux, macOS, BSD, and Windows, on epoll/kqueue via trantor. It
pulls jsoncpp, zlib, and optionally brotli, OpenSSL, and an ORM. There is no
NDK target. The vendored tree is already large, and `FEATURE_SERVE_DROGON`
exists so the main app can serve WebSockets and the image cache. That use
stays. VFS must not require it.

cpp-httplib is the server this repo already uses for `FEATURE_SERVE`, the MCP
embed, and the LLM agent server. One header, POSIX sockets, no extra JSON
library (we already have nlohmann), and it builds for the NDK. Range reads
and multipart upload are in the header. Chunked writes cover a task stream
without Drogon's SSE helpers.

On-device UI does not need HTTP at all. Android chat already lists a remote
Tanit mount through `service_vfs_list` (CMS bearer, HTTP client). A device
filesystem is a different call: JNI into `pm::vfs` on a directory the app is
allowed to read. Serving that tree on the LAN is a later `pm-vfs serve`,
bound to `127.0.0.1` unless `--lan` is set.

Civetweb and mongoose would also be small enough. They are new dependencies.
httplib is already fetched. Do not add another embedded server unless httplib
fails a concrete Android or range-request test.

The Drogon adapter, when we write it, is a set of controllers that call
`pm::vfs` and return JSON. No jail logic in the controller. If we drop
Drogon later, delete the adapter. The CLI and the httplib server stay.

## What the first build includes

Mount type `local` (alias `fs`) only. One mount root, `std::filesystem`,
symlink-checked.

In:

- Path sanitizer, ported to match `vfs/path-sanitizer.ts`: null bytes,
  double-encoding, `~`, absolute paths, `..` after decode, Windows reserved
  names, write-path allowlist.
- Jail: join under the mount root, `weakly_canonical` / `equivalent` prefix
  check, reject symlink escape. Missing parents are allowed only for create.
- Config file or `VFS_CONFIG` JSON array, same fields the TS loader reads:
  `name`, `type`, `path`, `isPublic`, `searchable`. Binds from a second file:
  `id`, `source.mount`, `source.subpath`, `target.mount`, `target.path`,
  `readonly`, `acl` (`source` | `inherit`).
- Ops: list, stat, read bytes, write bytes, upload (CLI: a local file path),
  mkdir, delete file or empty-or-recursive directory, copy/move inside the
  jail.
- Binds: longest matching target prefix, depth cap 8 (`EMLINK`), cycle
  (`ELOOP`), virtual directory rows merged into listings.
- Quota: `VFS_QUOTA_DEFAULT_MB`, `0` means unlimited. Counted only for a
  home write that is not a bind. Exceeded → `EDQUOT`.
- Errors as `{ "error", "code" }` with the TS codes: `ENOENT`, `EACCES`,
  `EFORBIDDEN`, `EFBIG`, `EDQUOT`, `ENOSPC`, `EEXIST`, `ELOOP`, `EMLINK`.

Out of the first build (later slices, still this package unless noted):

- `serve` and the `/api/vfs` route table.
- Signed GET (`sign-get`). Needs a host-supplied secret.
- Search, index, clear-index. The TS product reaches Postgres and a text
  index. Not a filesystem op.
- Image `?width=&format=` on GET. That is pm-image, called by the HTTP
  adapter, not by the jail.
- Username → user-id mount lookup. That is a host callback. Core only
  understands mount names, `home`, `user:{uuid}`, and a bare uuid under the
  `root` mount.
- Zip compress. Easy to add once list + read exist; not on the first CLI.
- `packages/acl.cpp`.

Home routes in the TS file (`/api/vfs/ls` with no `{mount}`) are sugar for
mount `home`. One C++ function takes the mount name. The router defaults it.

## Public library surface

Namespace `pm::vfs`. Headers under `include/pm/vfs/`. C++20.
`std::filesystem` only. No Win32 in the lib (the CLI may print paths).

```text
struct Error { std::string code; std::string message; };
struct Node  { name, path, size, mtime, mime, parent, type };  // VfsNode JSON

struct Caller { std::string user_id; };   // empty = anonymous

class Store {
  static Store open(Config);              // throws Error on a bad config
  std::vector<MountInfo> mounts(Caller) const;
  Result<std::vector<Node>> list(Caller, mount, subpath, ListOpts);
  Result<Node>               stat(Caller, mount, subpath);
  Result<Bytes>              read(Caller, mount, subpath, Range);
  Result<Node>               write(Caller, mount, subpath, bytes);
  Result<Node>               mkdir(Caller, mount, subpath);
  Result<void>               remove(Caller, mount, subpath, recursive);
  Result<CopyReport>         copy(Caller, CopyRequest);   // sync
};
```

`IVfsAuthorizer` is a virtual with one method,
`allow(caller, mount, subpath, op) -> Error|ok`. `AllowAll` is the default
and the only implementation in this package. `create` wraps the local
backend with the authorizer when one is installed. Public mounts
(`isPublic: true`) skip the authorizer for read and list, matching the TS
`createVFS` branch. With no ACL package linked, a non-public mount still
allows the caller: enforcement is opt-in, not a stub that denies everything.

Copy request fields match `VfsCopyRequest`: operation `copy|move|delete`,
sources, destination, `conflictStrategy`
(`error|overwrite|skip|rename|if_newer`), include/exclude globs. `manual`
conflict mode parks a `Task` and returns its id. The CLI blocks unless
`--async`.

## CLI

Binary name `pm-vfs` (`pm-vfs.exe` on Windows). Own `main`. Not a subcommand
of `tanit-cli`. Registration style follows `src/cli/pm_image_register_cli.cpp`:
one `register_*` function per command file, CLI11 subcommands, nlohmann on
stdout.

```text
pm-vfs --config vfs.json mounts
pm-vfs --config vfs.json ls home
pm-vfs --config vfs.json ls home photos --glob "*.jpg"
pm-vfs --config vfs.json stat home photos/a.jpg
pm-vfs --config vfs.json read home notes/a.txt
pm-vfs --config vfs.json write home notes/a.txt --data "hello"
pm-vfs --config vfs.json upload home inbox/a.bin --file ./a.bin
pm-vfs --config vfs.json mkdir home inbox
pm-vfs --config vfs.json rm home inbox/a.bin
pm-vfs --config vfs.json copy --json request.json
```

`--config` or `VFS_CONFIG` (inline JSON, same as the TS env). `--user` sets
the caller. `--json` is the default for every command; stdout is one JSON
value. Failure prints the error object and exits `1`. A usage error exits `2`.

`--config` and `--binds` are parent options, so they come before the subcommand:

```text
pm-vfs --config vfs.json --binds binds.json serve --host 127.0.0.1 --port 0
```

Port `0` prints the bound port as JSON and is what the HTTP tests use.

## HTTP route table (adapter contract)

When `serve` exists, paths match `api/vfs-routes.ts`. Home duplicates are the
same handlers with mount default `home`.

| method | path | lib call |
| --- | --- | --- |
| GET | `/api/vfs/mounts` | `mounts` |
| GET | `/api/vfs/ls/{mount}` and `.../*` | `list` |
| GET | `/api/vfs/stat/{mount}/*` | `stat` |
| GET | `/api/vfs/read/{mount}/*` | `read` as `text/plain` |
| GET | `/api/vfs/get/{mount}/*` | `read` with range, mime |
| PUT | `/api/vfs/write/{mount}/*` | `write` |
| POST | `/api/vfs/upload/{mount}/*` | `write` from multipart |
| DELETE | `/api/vfs/delete/{mount}/*` | `remove` |
| POST | `/api/vfs/mkdir/{mount}/*` | `mkdir` |
| POST | `/api/vfs/copy` | `copy` sync |
| POST | `/api/vfs/transfer` | `copy` async, `{taskId}` |
| GET | `/api/vfs/tasks/{id}` | task snapshot |
| POST | `/api/vfs/tasks/{id}/resolve` | conflict decision |
| GET | `/api/vfs/tasks/{id}/stream` | chunked task events |

Status mapping: `EFORBIDDEN`/`EACCES` 403, `ENOENT` 404, `EEXIST` 409,
`EFBIG` 413, `EDQUOT`/`ENOSPC` 507, anything else 400 or 500.

ACL routes (`/api/vfs/acl`, grant, revoke) return 404 until `packages/acl.cpp`
is linked. They are not stubbed inside `pm-vfs`.

## ACL package

`packages/acl.cpp` is a second static lib, `pm-acl`, built only when asked.

It owns `vfs-settings.json` (owner, groups, entries of user or group +
permissions `read|list|write|mkdir|delete`). It implements `IVfsAuthorizer`
and the grant/revoke mutations. `pm-vfs` stays free of that file format.

Link line, later, in the app or the CLI: `pm-vfs` + `pm-acl`. A CMake option
`PM_VFS_WITH_ACL` adds the authorizer and the three ACL routes to `serve`.
Default is off. Unit tests of the jail never set the option.

Username lookup and admin-quota bypass stay host callbacks. The TS product
asks Postgres for both. Pass them in from the app. The standalone CLI has
neither: quota is the env cap, and there is no username mount.

## Android

Ship the static lib in the NDK target. Do not ship Drogon there.

The device tree is a `local` mount whose `path` is chosen in Kotlin: an app
files directory, or a tree the user picked that Kotlin has already turned
into a real directory. `content://` URIs are not paths. The sheet copies or
takes a persistable tree, then calls C++.

New JNI, separate from `nativeServiceVfsList` (that one stays the remote CMS
client):

```text
nativeDeviceVfsList(root, relative) -> VfsNode JSON
nativeDeviceVfsRead(root, relative) -> bytes or a cache file path
```

Both call `Store` with one synthetic mount. No config file required on
device. LAN share, if we add it, is `pm-vfs serve` on httplib inside the
process, localhost unless the user opts into `--lan`.

## CMake

`packages/vfs.cpp/CMakeLists.txt` is a real project so this configures alone:

```text
cmake -S packages/vfs.cpp -B build/pm-vfs -DCMAKE_BUILD_TYPE=Release
cmake --build build/pm-vfs --target pm-vfs
```

When it is the top project it FetchContent's CLI11 and nlohmann (same tags
the root uses). When the root `add_subdirectory`s it, those targets already
exist and are reused. The lib target is `pm-vfs-lib` (output name `pm-vfs`
is the exe). Public include dir is `include/`.

Root wiring is a later todo, behind a cache option defaulting to OFF so a
normal app build does not compile this until we flip it:

```cmake
option(FEATURE_PM_VFS "Build packages/vfs.cpp into the app" OFF)
if(FEATURE_PM_VFS)
  add_subdirectory(packages/vfs.cpp)
endif()
```

Do not add the option to every preset in the same change as the lib.
Presets stay untouched until an app target actually links `pm-vfs-lib`.

`PM_VFS_HTTP` is empty by default (CLI + lib only). `httplib` turns on
`serve`. `drogon` is accepted only when a `drogon` CMake target already
exists (the root created it). This package never `add_subdirectory`s
`packages/drogon`.

Android toolchain: the lib has no server sources, so the NDK build of
`pm-vfs-lib` does not see httplib or drogon.

## Tests

The package owns its suites. `npm test` in `packages/vfs.cpp` runs them.
They were adapted from the pm-pics vitest chapters and from the local half of
`tests/orchestrator/test-vfs-copy.mjs`. `npm run test:vfs-copy` in the Tanit
repo still drives `tanit-cli` for ftp/ssh and the copy queue.

```text
npm run configure
npm run build:cpp
npm test
npm run test:copy
```

`PM_VFS_CLI` overrides the binary. The default is `build/dev/pm-vfs.exe`.

Cases, temp directory per test, config written beside it:

1. Sanitizer rejects: `..`, `%2e%2e`, `%252e`, `~`, `C:\`, NUL, double slash
   collapse still stays inside the root.
2. Symlink pointing outside the root is `EFORBIDDEN` on list/read/write.
3. `ls` / `stat` / `read` / `write` / `mkdir` / `rm` round trip and the
   JSON fields match `VfsNode` (`name`, `path`, `size`, `mime`, `parent`,
   `type`).
4. Write creates missing parents. Delete of a missing path is `ENOENT`.
5. Copy `error` on an existing dest is `EEXIST`. `overwrite` and `rename`
   do what the TS tests expect.
6. Bind: listing the host mount shows the virtual directory; a read through
   the bind path returns the source file; a cycle errors `ELOOP`.
7. Quota: small cap, write past it returns `EDQUOT`; a bind write does not
   count against the host home.
8. `isPublic` mount lists with an empty `--user`. A non-public mount also
   lists while ACL is off (documents the default). The deny case belongs
   to the ACL package's own script.
9. `serve` binds port 0, then `GET /api/vfs/ls`, `PUT` write, ranged `GET`,
   sync `POST /copy`, and a transfer that pauses for a manual conflict.
   `GET /tasks/{id}/stream` is NDJSON and ends on a terminal state.

`npm test` runs `tests/test-http.mjs` with the other suites.

## Layout to create

```text
packages/vfs.cpp/
  CMakeLists.txt
  CMakePresets.json
  package.json          configure, build:cpp, test, test:copy, ...
  config/               vfs.json, vfs-test.json, binds (paths stay inside the package)
  fixtures/             ch1, assets, chapter mounts
  include/pm/vfs/       error, types, store, tasks, http
  src/                  path, config, store, copy, tasks, http
  cli/main.cpp
  tests/                path, config, basic, copy, binds, quota, http
packages/acl.cpp/       not created until its todo
```

Todos 1–8 (skeleton through httplib serve and transfer tasks) are in this tree.
`npm test` in the package is the gate. httplib `serve` and transfer tasks are in.

## Todos

Work top to bottom. Each step keeps `npm test` in this package green before the
next one starts. Do not build the full app for these.

1. **Package skeleton.** Landed.
2. **Sanitizer + jail.** Landed.
3. **Config + local ops.** Landed.
4. **Copy.** Landed.
5. **Binds.** Landed.
6. **Quota.** Landed.
7. **httplib `serve`.** Landed. `src/http.cpp` is on the `pm-vfs` executable
   only. The static lib stays socket-free. Default host `127.0.0.1`.
8. **Tasks.** Landed. `transfer`, poll, `resolve`, chunked NDJSON stream.
9. **App link.** `FEATURE_PM_VFS` in the root `CMakeLists.txt` only, default
   OFF. One call site later (file panel or a `tanit-cli vfs` passthrough).
   Presets unchanged until that call site is real.
10. **`packages/acl.cpp`.** `IVfsAuthorizer`, `vfs-settings.json`, grant and
    revoke, `PM_VFS_WITH_ACL`. Own `test:pm-acl`. Deny test moves here.
11. **Android JNI.** `nativeDeviceVfsList` / `nativeDeviceVfsRead` over one
    local mount. Do not route it through Drogon or through
    `service_vfs_list`.
12. **Drogon adapter, only if a desktop server must share the existing
    Drogon process.** Controllers call `Store`. Skip this if httplib `serve`
    is enough for every host, including the main app.

Signed GET, search, zip, and image resize stay off this list until a caller
needs them.
