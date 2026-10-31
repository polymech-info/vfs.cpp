#pragma once

#include "pm/vfs/error.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pm::vfs {

struct Caller {
    std::string user_id;
};

struct Node {
    std::string name;
    std::string path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::string mime;
    std::string parent;
    std::string type;
};

struct MountInfo {
    std::string name;
};

struct Endpoint {
    std::string mount;
    std::string path;
};

struct Conflict {
    std::string source;
    std::string destination;
    std::string suggested_path;
};

struct CopyRequest {
    std::string operation = "copy";
    std::optional<Endpoint> source;
    std::vector<Endpoint> sources;
    Endpoint destination;
    std::string conflict_mode = "auto";
    std::string conflict_strategy = "error";
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    bool exclude_default = true;
    /// Manual mode: called on each conflict and returns the strategy to apply
    /// (`skip`, `overwrite`, `rename`, `if_newer`, `error`, `cancel`).
    std::function<std::string(const Conflict&)> on_conflict;
};

struct CopyReport {
    int files_copied = 0;
    int files_skipped = 0;
    std::uint64_t bytes_copied = 0;
    std::string destination_path;
    std::optional<Conflict> conflict;
};

struct ListResult {
    Error error;
    std::vector<Node> entries;
};

struct NodeResult {
    Error error;
    Node node;
};

struct ReadResult {
    Error error;
    int status = 200;
    std::string body;
    std::string mime;
    std::uint64_t content_length = 0;
    std::string content_range;
    std::string accept_ranges = "bytes";
};

struct CopyResult {
    Error error;
    CopyReport report;
};

struct QuotaConfig {
    double default_mb = 100;
    std::uint64_t default_bytes = 100ull * 1024ull * 1024ull;
    bool unlimited = false;
};

struct Range {
    bool active = false;
    bool suffix = false;
    bool unsatisfiable = false;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
};

} // namespace pm::vfs
