#include "pm/vfs/store.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace pm::vfs {
namespace {

namespace fs = std::filesystem;

fs::path resolve_against(const fs::path& base, const std::string& raw) {
    fs::path p(raw);
    if (p.empty()) return {};
    if (p.is_relative()) p = base / p;
    std::error_code ec;
    auto canon = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : canon;
}

Mount parse_mount(const nlohmann::json& j, const fs::path& base, Error& err) {
    Mount m;
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string() || !j.contains("path") || !j["path"].is_string()) {
        err = Error::make("EINVAL", "mount requires name and path");
        return m;
    }
    m.name = j["name"].get<std::string>();
    m.type = j.value("type", std::string("fs"));
    if (m.type != "fs" && m.type != "local") {
        err = Error::make("EINVAL", "unsupported mount type: " + m.type);
        return m;
    }
    m.root = resolve_against(base, j["path"].get<std::string>());
    m.is_public = j.value("isPublic", false);
    m.searchable = j.value("searchable", false);
    return m;
}

Bind parse_bind(const nlohmann::json& j, Error& err) {
    Bind b;
    if (!j.is_object()) {
        err = Error::make("EINVAL", "bind must be an object");
        return b;
    }
    b.id = j.value("id", std::string{});
    b.readonly = j.value("readonly", false);
    b.acl = j.value("acl", std::string{});
    if (!j.contains("source") || !j["source"].is_object() || !j.contains("target") || !j["target"].is_object()) {
        err = Error::make("EINVAL", "bind requires source and target");
        return b;
    }
    b.source_mount = j["source"].value("mount", std::string{});
    b.source_subpath = j["source"].value("subpath", std::string{});
    b.target_mount = j["target"].value("mount", std::string{});
    b.target_path = j["target"].value("path", std::string{});
    if (b.source_mount.empty() || b.target_mount.empty() || b.target_path.empty()) {
        err = Error::make("EINVAL", "bind source.mount, target.mount, and target.path are required");
        return b;
    }
    return b;
}

std::vector<Mount> parse_mounts(std::string_view json, const fs::path& base, Error& err) {
    std::vector<Mount> out;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const std::exception& ex) {
        err = Error::make("EINVAL", ex.what());
        return out;
    }
    if (!doc.is_array()) {
        err = Error::make("EINVAL", "mount config must be a JSON array");
        return out;
    }
    for (const auto& item : doc) {
        auto m = parse_mount(item, base, err);
        if (err) return {};
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<Bind> parse_binds(std::string_view json, Error& err) {
    std::vector<Bind> out;
    if (json.empty()) return out;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const std::exception& ex) {
        err = Error::make("EINVAL", ex.what());
        return out;
    }
    if (!doc.is_array()) {
        err = Error::make("EINVAL", "bind config must be a JSON array");
        return out;
    }
    for (const auto& item : doc) {
        auto b = parse_bind(item, err);
        if (err) return {};
        out.push_back(std::move(b));
    }
    return out;
}

std::string read_file(const fs::path& path, Error& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = Error::make("ENOENT", "config not found: " + path.string());
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

Store Store::open(const fs::path& config_path, const fs::path& binds_path, Error& err) {
    const auto text = read_file(config_path, err);
    if (err) return {};
    std::string binds;
    if (!binds_path.empty()) {
        binds = read_file(binds_path, err);
        if (err) return {};
    }
    // Mount paths resolve against the process cwd, same as the TS loader.
    return open_json(text, binds, fs::current_path(), err);
}

Store Store::open_json(std::string_view mounts_json, std::string_view binds_json,
                       const fs::path& base_dir, Error& err) {
    Store store;
    store.mounts_ = parse_mounts(mounts_json, base_dir, err);
    if (err) return {};
    store.binds_ = parse_binds(binds_json, err);
    if (err) return {};
    return store;
}

} // namespace pm::vfs
