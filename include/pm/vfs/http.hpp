#pragma once

#include "pm/vfs/store.hpp"

#include <string>

namespace pm::vfs {

/// Bind and serve `/api/vfs/*` on cpp-httplib. Prints `{"ok","host","port"}`
/// to stdout before accepting connections. `port` 0 picks an ephemeral port.
int serve_http(const Store& store, const std::string& host, int port);

} // namespace pm::vfs
