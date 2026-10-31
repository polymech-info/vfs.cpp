#include "pm/vfs/store.hpp"

#include "path_sanitizer.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <set>

namespace pm::vfs {
namespace {

namespace fs = std::filesystem;

const char* kDefaultExcludes[] = {
    "vfs-settings.json", ".git", ".svn", ".hg", "node_modules", ".npm", ".yarn",
    ".pnp.cjs", ".pnp.loader.mjs", "Thumbs.db", "desktop.ini", "ehthumbs.db",
    ".DS_Store", ".AppleDouble", ".LSOverride", ".Spotlight-V100", ".Trashes",
    ".fseventsd", ".TemporaryItems", "__MACOSX", ".vscode", ".idea", ".fleet",
    ".env", ".env.local", ".env.production", "zita.json", ".cache", ".turbo",
    ".next", ".nuxt", ".parcel-cache", ".tsbuildinfo", ".angular", ".svelte-kit",
    "__pycache__", ".venv", "venv", ".eggs", ".mypy_cache", ".pytest_cache",
    ".ruff_cache", "target", "coverage", ".nyc_output", "npm-debug.log",
    "yarn-debug.log", "yarn-error.log", ".terraform", ".vagrant", "bower_components",
    "CMakeFiles", "CMakeCache.txt", "cmake_install.cmake", "CMakeScripts", "out",
    "compile_commands.json", ".vs", "Debug", "Release", "x64", "x86", "ipch",
    "packages", "TestResults",
};

bool is_excluded_name(std::string_view name) {
    if (name.empty() || name[0] == '.') return true;
    for (const char* item : kDefaultExcludes)
        if (name == item) return true;
    return false;
}

bool path_excluded(std::string_view rel) {
    std::string seg;
    auto check = [&]() {
        if (!seg.empty() && is_excluded_name(seg)) return true;
        seg.clear();
        return false;
    };
    for (char c : rel) {
        if (c == '/') {
            if (check()) return true;
        } else {
            seg.push_back(c);
        }
    }
    return check();
}

std::string norm_generic(const fs::path& p) {
    std::string s = media::path::path_to_generic_utf8(p);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
#if defined(_WIN32)
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
    return s;
}

bool inside_root(const fs::path& root, const fs::path& candidate) {
    const auto r = norm_generic(root);
    const auto c = norm_generic(candidate);
    return c == r || c.rfind(r + "/", 0) == 0;
}

std::string trim_slashes(std::string s) {
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string join_rel2(std::string a, std::string b) {
    a = trim_slashes(std::move(a));
    b = trim_slashes(std::move(b));
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + "/" + b;
}

bool is_uuid(std::string_view s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

std::int64_t mtime_ms(const fs::path& p) {
    std::error_code ec;
    const auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    const auto delta = ft - fs::file_time_type::clock::now();
    const auto sys = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
}

std::string mime_for(std::string_view name, bool is_dir) {
    if (is_dir) return "inode/directory";
    const auto ext = media::strutil::path_ext_lower(name);
    if (ext == ".txt" || ext == ".md" || ext == ".log") return "text/plain";
    if (ext == ".json") return "application/json";
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".zip") return "application/zip";
    if (ext == ".pdf") return "application/pdf";
    return "application/octet-stream";
}

std::string parent_of(const std::string& rel) {
    const auto slash = rel.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return ".";
    return rel.substr(0, slash);
}

Node make_node(const std::string& rel, const fs::path& abs) {
    Node n;
    n.name = media::strutil::path_leaf(rel);
    n.path = rel;
    n.parent = parent_of(rel);
    std::error_code ec;
    const auto st = fs::symlink_status(abs, ec);
    const bool dir = !ec && (fs::is_directory(st) || (fs::is_symlink(st) && fs::is_directory(abs)));
    n.type = dir ? "dir" : "file";
    n.mime = mime_for(n.name, dir);
    if (!dir && !ec && fs::is_regular_file(fs::status(abs, ec)))
        n.size = static_cast<std::uint64_t>(fs::file_size(abs, ec));
    n.mtime = mtime_ms(abs);
    return n;
}

std::vector<std::string> gitignore_patterns(const fs::path& root, const fs::path& dir) {
    std::vector<std::string> patterns;
    std::vector<fs::path> chain;
    fs::path cur = dir;
    const auto root_n = norm_generic(root);
    while (true) {
        chain.push_back(cur);
        if (norm_generic(cur) == root_n) break;
        auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    std::reverse(chain.begin(), chain.end());
    for (const auto& d : chain) {
        std::ifstream in(d / ".gitignore");
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::size_t b = 0;
            while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
            if (b >= line.size() || line[b] == '#') continue;
            patterns.push_back(line.substr(b));
        }
    }
    return patterns;
}

bool ignored_by(const std::vector<std::string>& patterns, const std::string& rel, bool is_dir) {
    for (std::string pat : patterns) {
        bool dir_only = false;
        if (!pat.empty() && pat.back() == '/') {
            dir_only = true;
            pat.pop_back();
        }
        if (pat.empty()) continue;
        if (dir_only && !is_dir) continue;
        if (pat.find('/') == std::string::npos && pat.find('*') == std::string::npos && pat.find('?') == std::string::npos) {
            if (rel == pat) return true;
            const auto slash = rel.rfind('/');
            const auto name = slash == std::string::npos ? rel : rel.substr(slash + 1);
            if (name == pat) return true;
            continue;
        }
        if (match_glob(pat, rel)) return true;
    }
    return false;
}

std::uint64_t folder_bytes(const fs::path& root, const std::vector<std::string>& skip_rels) {
    if (!fs::exists(root)) return 0;
    std::uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto rel = media::path::path_to_generic_utf8(fs::relative(it->path(), root, ec));
        if (ec) continue;
        bool skip = false;
        for (const auto& s : skip_rels) {
            if (rel == s || rel.rfind(s + "/", 0) == 0) {
                skip = true;
                break;
            }
        }
        if (skip) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec))
            total += static_cast<std::uint64_t>(it->file_size(ec));
    }
    return total;
}

std::string env_or(const char* key) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string{};
}

} // namespace

QuotaConfig quota_config_from_env() {
    QuotaConfig cfg;
    const auto raw = env_or("VFS_QUOTA_DEFAULT_MB");
    if (!raw.empty()) {
        try {
            const double n = std::stod(raw);
            if (n >= 0) cfg.default_mb = n;
        } catch (...) {
        }
    }
    cfg.unlimited = cfg.default_mb == 0;
    if (cfg.unlimited) cfg.default_bytes = 0;
    else cfg.default_bytes = static_cast<std::uint64_t>(std::llround(cfg.default_mb * 1024.0 * 1024.0));
    return cfg;
}

bool would_exceed_quota(std::uint64_t used, std::uint64_t incoming, std::uint64_t limit, std::uint64_t replacing) {
    if (limit == 0) return false;
    if (incoming <= replacing) return false;
    const auto delta = incoming - replacing;
    return used + delta > limit;
}

bool is_home_quota_target(std::string_view mount, std::string_view user_id, bool is_bind) {
    if (is_bind) return false;
    if (user_id.empty()) return false;
    return mount == "home";
}

bool is_default_admin_id(std::string_view user_id) {
    if (user_id.empty()) return false;
    auto env = env_or("DEFAULT_ADMIN_ID");
    while (!env.empty() && (env.front() == ' ' || env.front() == '"')) env.erase(env.begin());
    while (!env.empty() && (env.back() == ' ' || env.back() == '"' || env.back() == '\'')) env.pop_back();
    while (!env.empty() && env.front() == '\'') env.erase(env.begin());
    return !env.empty() && env == user_id;
}

const Mount* Store::find_mount(std::string_view name) const {
    for (const auto& m : mounts_)
        if (m.name == name) return &m;
    return nullptr;
}

std::filesystem::path Store::jail(const fs::path& root, std::string_view rel, Error& err) const {
    const auto clean = sanitize_subpath(rel, err);
    if (err) return {};
    if (path_excluded(clean)) {
        err = Error::make("EACCES", "Forbidden");
        return {};
    }
    fs::path joined = clean.empty() ? root : root / fs::path(clean);
    std::error_code ec;
    fs::path canon;
    if (fs::exists(joined, ec)) {
        canon = fs::weakly_canonical(joined, ec);
    } else {
        const auto parent = joined.parent_path();
        auto parent_canon = fs::weakly_canonical(parent.empty() ? root : parent, ec);
        canon = clean.empty() ? parent_canon : parent_canon / joined.filename();
    }
    if (!inside_root(root, canon)) {
        err = Error::make("EFORBIDDEN", "path escapes mount root");
        return {};
    }
    return canon;
}

Store::Resolved Store::resolve(const Caller& caller, std::string_view mount_name, std::string_view subpath,
                               bool for_write, Error& err) const {
    Resolved out;
    out.requested_mount = std::string(mount_name);
    std::string sub(subpath);
    const auto clean = sanitize_subpath(sub, err);
    if (err) return {};
    sub = clean;

    const Mount* named = find_mount(mount_name);
    std::string owner;
    Mount host;
    std::string host_name(mount_name);
    if (named) {
        host = *named;
    } else if (mount_name == "home" || media::strutil::starts_with_ascii_ci(mount_name, "user:") || is_uuid(mount_name)) {
        if (caller.user_id.empty() && mount_name == "home") {
            err = Error::make("auth_required", "home mount requires authentication");
            err.status = 401;
            return {};
        }
        std::string uid = caller.user_id;
        if (media::strutil::starts_with_ascii_ci(mount_name, "user:")) uid = std::string(mount_name.substr(5));
        else if (is_uuid(mount_name)) uid = std::string(mount_name);
        if (uid.empty()) {
            err = Error::make("auth_required", "home mount requires authentication");
            err.status = 401;
            return {};
        }
        const Mount* root = find_mount("root");
        if (!root) {
            err = Error::make("ENOENT", "root mount is required for home");
            return {};
        }
        host = *root;
        host.name = host_name;
        host.root = root->root / uid;
        owner = uid;
    } else {
        err = Error::make("ENOENT", "Mount '" + std::string(mount_name) + "' not found");
        return {};
    }

    std::set<std::string> visited;
    std::string current_mount = host_name;
    Mount current = host;
    std::string current_sub = sub;
    std::string current_owner = owner;
    bool is_bind = false;
    bool readonly = false;

    for (int depth = 0; depth < 9; ++depth) {
        const Bind* best = nullptr;
        std::size_t best_len = 0;
        bool have = false;
        for (const auto& b : binds_) {
            if (b.target_mount != current_mount) continue;
            const auto bind_path = trim_slashes(b.target_path);
            if (current_sub == bind_path || current_sub.rfind(bind_path + "/", 0) == 0) {
                if (!have || bind_path.size() > best_len) {
                    best = &b;
                    best_len = bind_path.size();
                    have = true;
                }
            }
        }
        if (!best) break;
        if (visited.count(best->source_mount)) {
            err = Error::make("ELOOP", "ELOOP: Bind mount cycle detected");
            return {};
        }
        if (visited.size() >= 8) {
            err = Error::make("EMLINK", "EMLINK: Maximum bind mount depth exceeded");
            return {};
        }
        visited.insert(best->source_mount);
        const Mount* src = nullptr;
        Mount src_mount;
        std::string src_owner = current_owner;
        if (best->source_mount == "home" || media::strutil::starts_with_ascii_ci(best->source_mount, "user:")) {
            std::string uid = best->source_mount == "home" ? current_owner : best->source_mount.substr(5);
            const Mount* root = find_mount("root");
            if (!root || uid.empty()) {
                err = Error::make("ENOENT", "Bind source mount not found: " + best->source_mount);
                return {};
            }
            src_mount = *root;
            src_mount.root = root->root / uid;
            src_mount.name = best->source_mount;
            src = &src_mount;
            src_owner = uid;
        } else {
            src = find_mount(best->source_mount);
            if (!src) {
                err = Error::make("ENOENT", "Bind source mount not found: " + best->source_mount);
                return {};
            }
            src_mount = *src;
            src = &src_mount;
        }
        const auto bind_path = trim_slashes(best->target_path);
        std::string remaining = current_sub.size() == bind_path.size() ? std::string{} : current_sub.substr(bind_path.size() + 1);
        current_sub = join_rel2(best->source_subpath, remaining);
        current = src_mount;
        current_mount = best->source_mount;
        if (best->acl == "inherit") current_owner = owner;
        else current_owner = src_owner;
        is_bind = true;
        readonly = readonly || best->readonly;
        (void)src;
    }

    if (for_write && is_bind && readonly) {
        err = Error::make("EACCES", "readonly bind");
        return {};
    }
    out.mount = current;
    out.mount_name = current_mount;
    out.rel = current_sub;
    out.owner_id = current_owner.empty() ? caller.user_id : current_owner;
    out.is_bind = is_bind;
    out.readonly = readonly;
    out.requested_mount = std::string(mount_name);
    return out;
}

bool Store::quota_blocks(const Caller& caller, const Resolved& resolved, std::uint64_t incoming,
                         std::uint64_t replacing) const {
    if (!is_home_quota_target(resolved.requested_mount, caller.user_id, resolved.is_bind)) return false;
    if (is_default_admin_id(caller.user_id)) return false;
    const auto cfg = quota_config_from_env();
    if (cfg.unlimited) return false;
    std::vector<std::string> skip;
    for (const auto& b : binds_) {
        if (b.target_mount == "home") skip.push_back(trim_slashes(b.target_path));
    }
    const auto home_root = resolved.is_bind ? fs::path{} : resolved.mount.root;
    // Quota is counted on the caller's home directory, not on a bind source.
    const Mount* root = find_mount("root");
    fs::path measure = root ? root->root / caller.user_id : home_root;
    const auto used = folder_bytes(measure, skip);
    return would_exceed_quota(used, incoming, cfg.default_bytes, replacing);
}

std::vector<MountInfo> Store::mounts() const {
    std::vector<MountInfo> out;
    out.reserve(mounts_.size());
    for (const auto& m : mounts_) out.push_back(MountInfo{m.name});
    return out;
}

ListResult Store::list(const Caller& caller, std::string_view mount, std::string_view subpath,
                       std::string_view glob) const {
    ListResult result;
    Error err;
    auto resolved = resolve(caller, mount, subpath, false, err);
    if (err) {
        result.error = err;
        return result;
    }
    auto abs = jail(resolved.mount.root, resolved.rel, err);
    if (err) {
        result.error = err;
        return result;
    }
    std::error_code ec;
    if (!fs::exists(abs, ec)) {
        result.error = Error::make("ENOENT", "Not found");
        return result;
    }
    if (!fs::is_directory(abs, ec)) {
        result.error = Error::make("ENOTDIR", "Not found");
        return result;
    }
    const auto patterns = gitignore_patterns(resolved.mount.root, abs);
    for (const auto& entry : fs::directory_iterator(abs, ec)) {
        const auto name = media::path::path_to_utf8(entry.path().filename());
        if (is_excluded_name(name)) continue;
        const auto rel = join_rel2(resolved.rel, name);
        std::error_code sec;
        const bool dir = entry.is_directory(sec);
        if (ignored_by(patterns, rel, dir)) continue;
        if (!glob.empty() && !match_glob(glob, name)) continue;
        if (entry.is_symlink(sec)) {
            auto canon = fs::weakly_canonical(entry.path(), sec);
            if (!inside_root(resolved.mount.root, canon)) continue;
        }
        result.entries.push_back(make_node(rel, entry.path()));
    }
    if (!resolved.is_bind) {
        std::set<std::string> names;
        for (const auto& n : result.entries) names.insert(n.name);
        const auto here = trim_slashes(resolved.rel);
        for (const auto& b : binds_) {
            if (b.target_mount != resolved.requested_mount && b.target_mount != resolved.mount_name) continue;
            // Virtual rows are shown on the mount the caller asked for, before a bind swallows the path.
            if (b.target_mount != std::string(mount) && b.target_mount != resolved.requested_mount) continue;
            const auto bind_path = trim_slashes(b.target_path);
            std::string child;
            if (here.empty()) {
                child = bind_path.substr(0, bind_path.find('/'));
            } else if (bind_path.rfind(here + "/", 0) == 0) {
                auto rest = bind_path.substr(here.size() + 1);
                child = rest.substr(0, rest.find('/'));
            }
            if (child.empty() || names.count(child)) continue;
            if (!glob.empty() && !match_glob(glob, child)) continue;
            Node n;
            n.name = child;
            n.path = join_rel2(here, child);
            n.parent = here.empty() ? "." : here;
            n.type = "dir";
            n.mime = "inode/directory";
            n.mtime = 0;
            result.entries.push_back(n);
            names.insert(child);
        }
    }
    std::sort(result.entries.begin(), result.entries.end(),
              [](const Node& a, const Node& b) { return a.name < b.name; });
    return result;
}

NodeResult Store::stat(const Caller& caller, std::string_view mount, std::string_view subpath) const {
    NodeResult result;
    Error err;
    auto resolved = resolve(caller, mount, subpath, false, err);
    if (err) {
        result.error = std::move(err);
        return result;
    }
    auto abs = jail(resolved.mount.root, resolved.rel, err);
    if (err) {
        result.error = std::move(err);
        return result;
    }
    std::error_code ec;
    if (!fs::exists(abs, ec)) {
        result.error = Error::make("ENOENT", "Not found");
        return result;
    }
    result.node = make_node(resolved.rel.empty() ? std::string(resolved.mount.name) : resolved.rel, abs);
    if (resolved.rel.empty()) result.node.name = resolved.mount.name;
    return result;
}

ReadResult Store::read(const Caller& caller, std::string_view mount, std::string_view subpath, const Range& range) const {
    ReadResult result;
    Error err;
    auto resolved = resolve(caller, mount, subpath, false, err);
    if (err) {
        result.error = std::move(err);
        result.status = result.error.status;
        return result;
    }
    auto abs = jail(resolved.mount.root, resolved.rel, err);
    if (err) {
        result.error = std::move(err);
        result.status = result.error.status;
        return result;
    }
    std::error_code ec;
    if (!fs::exists(abs, ec)) {
        result.error = Error::make("ENOENT", "Not found");
        result.status = 404;
        return result;
    }
    if (fs::is_directory(abs, ec)) {
        result.error = Error::make("EISDIR", "Path is a directory");
        result.status = 400;
        return result;
    }
    std::ifstream in(abs, std::ios::binary);
    if (!in) {
        result.error = Error::make("EACCES", "Forbidden");
        result.status = 403;
        return result;
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    result.content_length = data.size();
    result.mime = mime_for(media::path::path_to_utf8(abs.filename()), false);
    if (!range.active) {
        result.body = std::move(data);
        result.status = 200;
        return result;
    }
    if (range.unsatisfiable || (data.empty() && range.active)) {
        result.error = Error::make("EINVAL", "Range not satisfiable");
        result.error.status = 416;
        result.status = 416;
        result.content_range = "bytes */" + std::to_string(data.size());
        result.body.clear();
        return result;
    }
    const auto start = std::min(range.start, static_cast<std::uint64_t>(data.size()));
    auto end = std::min(range.end, data.empty() ? 0 : static_cast<std::uint64_t>(data.size() - 1));
    if (start > end || start >= data.size()) {
        result.error = Error::make("EINVAL", "Range not satisfiable");
        result.error.status = 416;
        result.status = 416;
        result.content_range = "bytes */" + std::to_string(data.size());
        return result;
    }
    result.body = data.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start + 1));
    result.status = 206;
    result.content_length = result.body.size();
    result.content_range = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(data.size());
    return result;
}

NodeResult Store::write(const Caller& caller, std::string_view mount, std::string_view subpath, std::string_view bytes) const {
    NodeResult result;
    Error err;
    const auto clean = sanitize_subpath(subpath, err);
    if (err) {
        result.error = err;
        return result;
    }
    const auto written = sanitize_write_path(clean, false);
    auto resolved = resolve(caller, mount, written, true, err);
    if (err) {
        result.error = std::move(err);
        return result;
    }
    if (resolved.rel.empty()) {
        result.error = Error::make("EISDIR", "Path is a directory");
        return result;
    }
    auto abs = jail(resolved.mount.root, resolved.rel, err);
    if (err) {
        result.error = std::move(err);
        return result;
    }
    std::error_code ec;
    std::uint64_t replacing = 0;
    if (fs::is_regular_file(abs, ec)) replacing = static_cast<std::uint64_t>(fs::file_size(abs, ec));
    if (quota_blocks(caller, resolved, bytes.size(), replacing)) {
        result.error = Error::make("EDQUOT", "Quota exceeded");
        return result;
    }
    fs::create_directories(abs.parent_path(), ec);
    if (ec) {
        result.error = Error::make("EACCES", ec.message());
        return result;
    }
    std::ofstream out(abs, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.error = Error::make("EACCES", "Forbidden");
        return result;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        result.error = Error::make("EIO", "write failed");
        return result;
    }
    result.node = make_node(resolved.rel, abs);
    return result;
}

NodeResult Store::mkdir(const Caller& caller, std::string_view mount, std::string_view subpath) const {
    NodeResult result;
    Error err;
    auto clean_in = sanitize_subpath(subpath, err);
    if (err) {
        result.error = std::move(err);
        return result;
    }
    const auto written = sanitize_write_path(clean_in, true);
    auto resolved = resolve(caller, mount, written, true, err);
    if (err) {
        result.error = std::move(err);
        return result;
    }
    auto abs = jail(resolved.mount.root, resolved.rel, err);
    if (err) {
        result.error = std::move(err);
        return result;
    }
    std::error_code ec;
    fs::create_directories(abs, ec);
    if (ec) {
        result.error = Error::make("EACCES", ec.message());
        return result;
    }
    result.node = make_node(resolved.rel, abs);
    return result;
}

Error Store::remove(const Caller& caller, std::string_view mount, std::string_view subpath, bool recursive) const {
    Error err;
    auto resolved = resolve(caller, mount, subpath, true, err);
    if (err) return err;
    auto abs = jail(resolved.mount.root, resolved.rel, err);
    if (err) return err;
    std::error_code ec;
    if (!fs::exists(abs, ec)) return Error::make("ENOENT", "Not found");
    if (fs::is_directory(abs, ec) && !recursive) {
        fs::remove(abs, ec);
        if (ec) return Error::make("EACCES", ec.message());
        return Error::ok();
    }
    fs::remove_all(abs, ec);
    if (ec) return Error::make("EACCES", ec.message());
    return Error::ok();
}

} // namespace pm::vfs
