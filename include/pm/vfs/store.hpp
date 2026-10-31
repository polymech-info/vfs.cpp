#pragma once

#include "pm/vfs/types.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pm::vfs {

struct Mount {
    std::string name;
    std::string type;
    std::filesystem::path root;
    bool is_public = false;
    bool searchable = false;
};

struct Bind {
    std::string id;
    std::string source_mount;
    std::string source_subpath;
    std::string target_mount;
    std::string target_path;
    bool readonly = false;
    std::string acl;
};

class Store {
public:
    static Store open(const std::filesystem::path& config_path,
                      const std::filesystem::path& binds_path,
                      Error& err);

    static Store open_json(std::string_view mounts_json,
                           std::string_view binds_json,
                           const std::filesystem::path& base_dir,
                           Error& err);

    std::vector<MountInfo> mounts() const;

    ListResult list(const Caller& caller, std::string_view mount, std::string_view subpath,
                    std::string_view glob = {}) const;
    NodeResult stat(const Caller& caller, std::string_view mount, std::string_view subpath) const;
    ReadResult read(const Caller& caller, std::string_view mount, std::string_view subpath,
                    const Range& range = {}) const;
    NodeResult write(const Caller& caller, std::string_view mount, std::string_view subpath,
                     std::string_view bytes) const;
    NodeResult mkdir(const Caller& caller, std::string_view mount, std::string_view subpath) const;
    Error remove(const Caller& caller, std::string_view mount, std::string_view subpath,
                 bool recursive) const;
    CopyResult copy(const Caller& caller, const CopyRequest& request) const;

    const std::vector<Mount>& mount_table() const { return mounts_; }
    const std::vector<Bind>& bind_table() const { return binds_; }

private:
    struct Resolved {
        Mount mount;
        std::string mount_name;
        std::string rel;
        std::string owner_id;
        bool is_bind = false;
        bool readonly = false;
        std::string requested_mount;
    };

    Resolved resolve(const Caller& caller, std::string_view mount, std::string_view subpath,
                     bool for_write, Error& err) const;
    std::filesystem::path jail(const std::filesystem::path& root, std::string_view rel, Error& err) const;
    const Mount* find_mount(std::string_view name) const;
    bool quota_blocks(const Caller& caller, const Resolved& resolved, std::uint64_t incoming,
                      std::uint64_t replacing) const;

    std::vector<Mount> mounts_;
    std::vector<Bind> binds_;
};

QuotaConfig quota_config_from_env();
bool would_exceed_quota(std::uint64_t used, std::uint64_t incoming, std::uint64_t limit,
                        std::uint64_t replacing = 0);
bool is_home_quota_target(std::string_view mount, std::string_view user_id, bool is_bind);
bool is_default_admin_id(std::string_view user_id);

std::string sanitize_subpath(std::string_view input, Error& err);
std::string sanitize_write_path(std::string_view subpath, bool is_directory);
bool match_glob(std::string_view pattern, std::string_view text);

Range parse_range(std::string_view text, std::uint64_t size);

} // namespace pm::vfs
