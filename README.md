# pm-vfs

A portable virtual filesystem. One C++ library jails a set of directories, then lists, reads, writes, and copies inside that jail. Desktop, a small HTTP server, and a future Android build all call the same code.

The library has no sockets and no auth product. A host picks how to reach it:

| host | how it calls the jail |
| --- | --- |
| this package's CLI | `pm-vfs` subcommands, JSON on stdout |
| a process on the LAN or localhost | `pm-vfs serve` (`/api/vfs` on cpp-httplib) |
| Tanit desktop, later | `add_subdirectory` and `pm::vfs::Store` |
| Android, later | JNI into `Store` on a directory the app is allowed to read |

`src/vfs` in the Tanit repo stays the desktop **client** (`vfs://`, ftp, ssh, mtp, `tanit-cli cp`). This package is the **jail** that client can talk to. `npm run test:vfs-copy` in the Tanit repo still drives `tanit-cli`. The suites in `tests/` here drive `pm-vfs`.

ACL is a later package (`packages/acl.cpp`). Until it exists, every mount is readable and writable by whoever can run the process. Quota is the one limit that is already on: an env cap on `home` writes.

Design notes and the ordered follow-ups live in [design.md](design.md).

## Why a separate layer

The TypeScript server in pm-pics already defines the behavior: mount names, binds, a path jail, copy conflicts, and `/api/vfs`. That server sits on Hono and assumes Node. The same rules need to run where Node does not:

- a Windows or Linux desktop process, linked from source
- a phone process, through JNI, on an app-private directory
- a tiny localhost server for tests and tools, without pulling in Drogon

Drogon stays in the Tanit tree for other desktop servers. It is a poor fit for the NDK (no Android target, and it brings trantor, jsoncpp, and zlib). The VFS core therefore does not include an HTTP stack. `serve` is a cpp-httplib front on the `pm-vfs` executable only. The static library `pm-vfs-lib` is what Android and the main app link.

A caller is a `user_id` string. Empty means anonymous. The CLI passes `--user`. An HTTP host can pass `X-Vfs-User`. The library does not look up users.

## Build

From `packages/vfs.cpp` (or `npm run build:cpp:vfs` from the Tanit root):

```text
npm run configure
npm run build:cpp
npm test
```

The binary is `build/dev/pm-vfs.exe` on Windows and `build/dev/pm-vfs` elsewhere. `PM_VFS_CLI` overrides the path the tests spawn. Presets are `dev` (RelWithDebInfo) and `release`.

```text
npm run build:cpp-fast      # skip configure
npm run build:cpp:release
npm run test:path           # one suite
npm run test:http
```

Configs and fixtures stay inside this package. Paths in the JSON files are relative to the process working directory, which for the commands below is `packages/vfs.cpp`.

## Setup: one directory

`config/local.json`:

```json
[
  { "name": "work", "type": "fs", "path": "D:/jobs/acme", "searchable": false }
]
```

`type` is `fs` or `local`. `path` is resolved against the current directory, then checked with a real path so a symlink cannot leave the root.

```text
pm-vfs --config config/local.json mounts
pm-vfs --config config/local.json ls work
pm-vfs --config config/local.json ls work drawings --glob "*.pdf"
pm-vfs --config config/local.json read work notes/a.txt
pm-vfs --config config/local.json write work notes/a.txt --data "hello"
pm-vfs --config config/local.json get work notes/a.txt --range bytes=0-4
pm-vfs --config config/local.json mkdir work inbox
pm-vfs --config config/local.json rm work inbox -r
```

`--config` and `--binds` belong to the program, so they come before the subcommand. Success is one JSON object on stdout and exit 0. Failure is `{"ok":false,"error","code","status"}` and exit 1. A usage error exits 2.

Dotfiles, `vfs-settings.json`, and the usual build directories (`node_modules`, `.git`, …) stay out of listings. A `.gitignore` in the tree hides matching names too.

## Setup: several mounts and a bind

A bind makes one mount appear as a directory inside another. The checked-in pair `config/vfs.json` and `config/vfs-binds.json` does this for fixtures:

```json
[
  { "name": "root", "type": "fs", "path": "fixtures/vfs" },
  { "name": "assets", "type": "fs", "path": "fixtures/vfs-assets" }
]
```

```json
[
  {
    "id": "root-assets",
    "source": { "mount": "assets" },
    "target": { "mount": "root", "path": "assets" },
    "acl": "source"
  }
]
```

`ls root` then shows a virtual directory `assets`. Reading `root/assets/marker.txt` returns the file that lives on the `assets` mount. The longest matching target prefix wins. A cycle returns `ELOOP`. Depth past 8 returns `EMLINK`. `"readonly": true` allows the read and rejects the write.

```text
pm-vfs --config config/vfs.json --binds config/vfs-binds.json ls root
pm-vfs --config config/vfs.json --binds config/vfs-binds.json read root assets/marker.txt
```

`npm run serve` uses the larger test pair, `config/vfs-test.json` and `config/vfs-binds-test.json` (firmware under machines, a deep bind, a readonly shared folder).

## Setup: a per-user home

`home` is not its own root. It is `root/<user id>` on the mount named `root`. The caller has to be set, or the call is `401 auth_required`.

```text
pm-vfs --config config/vfs-test.json --user user-1 mkdir home inbox
pm-vfs --config config/vfs-test.json --user user-1 write home inbox/a.txt --data "mine"
```

`user:<uuid>` and a bare uuid are the same folder under `root`. `VFS_QUOTA_DEFAULT_MB` caps bytes written to that home (default 100, `0` means unlimited). A write that arrives through a bind does not count. `DEFAULT_ADMIN_ID` skips the cap for that user id.

## Setup: copy and move

```text
pm-vfs --config config/vfs-test.json copy --src-mount ch1 --src hello.txt --dst-mount ch1 --dst copies/hello.txt --strategy overwrite
```

`--operation` is `copy` (default), `move`, or `delete`. `--strategy` is `error`, `overwrite`, `skip`, `rename`, or `if_newer`. `rename` writes `name (copy 1).ext` beside the existing file. `--mode manual` on the CLI returns `409` with `conflict.suggestedPath` and does not wait. A background transfer is the HTTP `transfer` route below.

The same request as a file, passed with `--request`:

```json
{
  "operation": "copy",
  "source": { "mount": "ch1", "path": "hello.txt" },
  "destination": { "mount": "ch1", "path": "copies/hello.txt" },
  "conflictMode": "auto",
  "conflictStrategy": "overwrite"
}
```

`--include` and `--exclude` take globs (`*`, `**`). Default excludes (dotfiles, `node_modules`, …) apply unless `--no-exclude-default` is set. Copying a directory places its children into the destination, not into `destination/basename`.

## Setup: HTTP

`serve` prints one line, then blocks:

```json
{"ok":true,"host":"127.0.0.1","port":8787}
```

From this package:

```text
npm run serve
```

That is the test layout on `127.0.0.1:8787`. `PM_VFS_HOST` and `PM_VFS_PORT` change the address. Arguments after `--` are passed through to `pm-vfs`.

From the Tanit root: `npm run serve:vfs`.

Any other config, still from `packages/vfs.cpp`:

```text
build/dev/pm-vfs --config config/vfs.json --binds config/vfs-binds.json serve --host 127.0.0.1 --port 8787
```

Port `0` picks an ephemeral port and prints it. The default bind address is localhost.

```text
GET    /api/vfs/mounts
GET    /api/vfs/ls/ch1
GET    /api/vfs/ls/ch1/sub?glob=*.txt
GET    /api/vfs/stat/ch1/hello.txt
GET    /api/vfs/read/ch1/hello.txt
GET    /api/vfs/get/ch1/hello.txt          Range: bytes=0-4   → 206
PUT    /api/vfs/write/ch1/note.txt         body is the file bytes
POST   /api/vfs/upload/ch1/note.txt        multipart field "file"
POST   /api/vfs/mkdir/ch1/inbox
DELETE /api/vfs/delete/ch1/inbox
POST   /api/vfs/copy                       JSON body, same shape as --request
POST   /api/vfs/transfer                   JSON body → {"taskId"}
GET    /api/vfs/tasks/{id}
POST   /api/vfs/tasks/{id}/resolve         {"decision":"skip"}
GET    /api/vfs/tasks/{id}/stream          NDJSON until completed, failed, or cancelled
```

List results are a JSON array. Errors are `{"error","code"}` with the status mapped from the code (`ENOENT` 404, `EACCES` 403, `EEXIST` 409, `EDQUOT` 507). A transfer in `manual` mode pauses in `paused_for_conflict` until `resolve` sends `skip`, `overwrite`, `rename`, `if_newer`, `error`, or `cancel`. `skip_all` and `overwrite_all` stick for the rest of that task.

Send `X-Vfs-User: user-1` when the path is `home`.

## Setup: link the library

The static library is the piece another app should use. HTTP stays on the executable.

```cmake
add_subdirectory(packages/vfs.cpp)
target_link_libraries(my_app PRIVATE pm-vfs-lib)
```

```cpp
#include "pm/vfs/store.hpp"

pm::vfs::Error err;
auto store = pm::vfs::Store::open("vfs.json", "vfs-binds.json", err);
if (err) { /* err.code, err.message, err.status */ }

pm::vfs::Caller me;
me.user_id = "user-1";
auto listed = store.list(me, "root", "", "");
auto text = store.read(me, "root", "readme.txt", {});
```

Headers are under `include/pm/vfs/`. Namespace `pm::vfs`. C++20.

`src/commons/` holds a copy of the shared string helpers and path sanitizer so this package configures alone. That copy moves to its own package later. The jail calls `sanitize_subpath` and `sanitize_write_path` from there.

## What is not in this tree yet

Signed download URLs, search, zip export, and image resize on GET. Username to uuid lookup. An ACL package. A Drogon adapter. Wiring into the Tanit root build (`FEATURE_PM_VFS` stays off until a real call site exists). Android JNI.

Those are listed, in order, in [design.md](design.md).
