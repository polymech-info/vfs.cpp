#include "pm/vfs/http.hpp"
#include "pm/vfs/store.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using pm::vfs::Caller;
using pm::vfs::CopyRequest;
using pm::vfs::Endpoint;
using pm::vfs::Error;
using pm::vfs::Store;

nlohmann::json error_json(const Error& err) {
    nlohmann::json j;
    j["ok"] = false;
    j["status"] = err.status ? err.status : pm::vfs::vfs_errno_status(err.code);
    j["code"] = pm::vfs::canonical_vfs_errno(err.code);
    j["error"] = err.message.empty() ? pm::vfs::default_vfs_message(err.code) : err.message;
    return j;
}

nlohmann::json node_json(const pm::vfs::Node& n) {
    return {
        {"name", n.name},
        {"path", n.path},
        {"size", n.size},
        {"mtime", n.mtime},
        {"mime", n.mime},
        {"parent", n.parent},
        {"type", n.type},
    };
}

void emit(const nlohmann::json& j, bool ok) {
    std::cout << j.dump() << "\n";
    std::exit(ok ? 0 : 1);
}

void emit_error(const Error& err) {
    auto j = error_json(err);
    if (!err.message.empty()) j["error"] = err.message;
    emit(j, false);
}

Store load_store(const std::string& config, const std::string& binds) {
    Error err;
    if (!config.empty()) {
        auto store = Store::open(config, binds, err);
        if (err) emit_error(err);
        return store;
    }
    const char* env = std::getenv("VFS_CONFIG");
    if (!env) emit_error(Error::make("EINVAL", "pass --config or set VFS_CONFIG"));
    auto store = Store::open_json(env, {}, std::filesystem::current_path(), err);
    if (err) emit_error(err);
    return store;
}

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Endpoint endpoint_from(const nlohmann::json& j) {
    Endpoint e;
    if (j.is_object()) {
        e.mount = j.value("mount", std::string{});
        e.path = j.value("path", std::string{});
    }
    return e;
}

CopyRequest request_from_json(const nlohmann::json& j) {
    CopyRequest req;
    req.operation = j.value("operation", std::string("copy"));
    req.conflict_mode = j.value("conflictMode", std::string("auto"));
    req.conflict_strategy = j.value("conflictStrategy", std::string("error"));
    req.exclude_default = j.value("excludeDefault", true);
    if (j.contains("source")) req.source = endpoint_from(j["source"]);
    if (j.contains("destination")) req.destination = endpoint_from(j["destination"]);
    if (j.contains("sources") && j["sources"].is_array()) {
        for (const auto& item : j["sources"]) req.sources.push_back(endpoint_from(item));
    }
    if (j.contains("includePatterns") && j["includePatterns"].is_array()) {
        for (const auto& item : j["includePatterns"])
            if (item.is_string()) req.include.push_back(item.get<std::string>());
    }
    if (j.contains("excludePatterns") && j["excludePatterns"].is_array()) {
        for (const auto& item : j["excludePatterns"])
            if (item.is_string()) req.exclude.push_back(item.get<std::string>());
    }
    return req;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"pm-vfs"};
    app.require_subcommand(1);
    std::string config;
    std::string binds;
    std::string user;
    app.add_option("--config", config, "Mount JSON file");
    app.add_option("--binds", binds, "Bind JSON file");
    app.add_option("--user", user, "Caller user id");

    auto* mounts_cmd = app.add_subcommand("mounts", "List mount names");

    std::string ls_mount, ls_path, ls_glob;
    auto* ls = app.add_subcommand("ls", "List a directory");
    ls->add_option("mount", ls_mount, "Mount name")->required();
    ls->add_option("path", ls_path, "Relative path");
    ls->add_option("--glob", ls_glob, "Filename glob");

    std::string stat_mount, stat_path;
    auto* stat_cmd = app.add_subcommand("stat", "Stat a path");
    stat_cmd->add_option("mount", stat_mount)->required();
    stat_cmd->add_option("path", stat_path, "Relative path")->required();

    std::string read_mount, read_path;
    auto* read_cmd = app.add_subcommand("read", "Read a file as text");
    read_cmd->add_option("mount", read_mount)->required();
    read_cmd->add_option("path", read_path)->required();

    std::string get_mount, get_path, get_range;
    auto* get_cmd = app.add_subcommand("get", "Read a file with an optional byte range");
    get_cmd->add_option("mount", get_mount)->required();
    get_cmd->add_option("path", get_path)->required();
    get_cmd->add_option("--range", get_range, "bytes=start-end or start-end or -N");

    std::string write_mount, write_path, write_data, write_file;
    auto* write_cmd = app.add_subcommand("write", "Write a file");
    write_cmd->add_option("mount", write_mount)->required();
    write_cmd->add_option("path", write_path)->required();
    write_cmd->add_option("--data", write_data, "UTF-8 contents");
    write_cmd->add_option("--file", write_file, "Copy bytes from this local file");

    std::string mkdir_mount, mkdir_path;
    auto* mkdir_cmd = app.add_subcommand("mkdir", "Create a directory");
    mkdir_cmd->add_option("mount", mkdir_mount)->required();
    mkdir_cmd->add_option("path", mkdir_path)->required();

    std::string rm_mount, rm_path;
    bool rm_recursive = false;
    auto* rm_cmd = app.add_subcommand("rm", "Remove a file or directory");
    rm_cmd->add_option("mount", rm_mount)->required();
    rm_cmd->add_option("path", rm_path)->required();
    rm_cmd->add_flag("-r,--recursive", rm_recursive);

    std::string copy_request;
    std::string copy_op = "copy";
    std::string copy_strategy = "error";
    std::string copy_mode = "auto";
    std::string src_mount, dst_mount, dst_path;
    std::vector<std::string> src_paths, includes, excludes;
    bool no_exclude_default = false;
    auto* copy_cmd = app.add_subcommand("copy", "Copy, move, or delete");
    copy_cmd->add_option("--request", copy_request, "JSON body file");
    copy_cmd->add_option("--operation", copy_op, "copy|move|delete");
    copy_cmd->add_option("--strategy", copy_strategy, "error|overwrite|skip|rename|if_newer");
    copy_cmd->add_option("--mode", copy_mode, "auto|manual");
    copy_cmd->add_option("--src-mount", src_mount);
    copy_cmd->add_option("--src", src_paths);
    copy_cmd->add_option("--dst-mount", dst_mount);
    copy_cmd->add_option("--dst", dst_path);
    copy_cmd->add_option("--include", includes);
    copy_cmd->add_option("--exclude", excludes);
    copy_cmd->add_flag("--no-exclude-default", no_exclude_default);

    std::string check_input;
    bool check_write = false;
    bool check_dir = false;
    auto* check_cmd = app.add_subcommand("check-path", "Sanitize a relative path");
    check_cmd->add_option("--input", check_input, "Path to sanitize")->required();
    check_cmd->add_flag("--write", check_write);
    check_cmd->add_flag("--dir", check_dir);

    std::string errno_code, errno_guess;
    bool errno_canonical = false;
    auto* errno_cmd = app.add_subcommand("errno", "Map a VFS errno to HTTP");
    errno_cmd->add_option("code", errno_code, "Errno token");
    errno_cmd->add_option("--guess", errno_guess, "Pull an errno out of a message");
    errno_cmd->add_flag("--canonical", errno_canonical);

    double quota_used = -1, quota_incoming = -1, quota_limit = -1, quota_replacing = 0;
    std::string quota_mount, quota_user, quota_admin;
    bool quota_bind = false;
    bool quota_home = false;
    auto* quota_cmd = app.add_subcommand("quota", "Quota config and predicates");
    quota_cmd->add_option("--used", quota_used);
    quota_cmd->add_option("--incoming", quota_incoming);
    quota_cmd->add_option("--limit", quota_limit);
    quota_cmd->add_option("--replacing", quota_replacing);
    quota_cmd->add_option("--mount", quota_mount);
    quota_cmd->add_option("--owner", quota_user);
    quota_cmd->add_flag("--bind", quota_bind);
    quota_cmd->add_flag("--home-target", quota_home);
    quota_cmd->add_option("--admin-id", quota_admin);

    std::string serve_host = "127.0.0.1";
    int serve_port = 0;
    auto* serve_cmd = app.add_subcommand("serve", "HTTP /api/vfs on 127.0.0.1");
    serve_cmd->add_option("--host", serve_host, "Bind address");
    serve_cmd->add_option("--port", serve_port, "Bind port, 0 for ephemeral");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    Caller caller;
    caller.user_id = user;

    if (errno_cmd->parsed()) {
        nlohmann::json j;
        j["ok"] = true;
        if (!errno_guess.empty()) {
            const auto guessed = pm::vfs::guess_vfs_errno(errno_guess);
            if (guessed.empty()) j["code"] = nullptr;
            else j["code"] = guessed;
            emit(j, true);
        }
        if (errno_canonical) {
            j["code"] = pm::vfs::canonical_vfs_errno(errno_code);
            emit(j, true);
        }
        const auto mapped = pm::vfs::map_vfs_errno(errno_code);
        j["status"] = mapped.status;
        j["error"] = mapped.error;
        j["code"] = mapped.code;
        emit(j, true);
    }

    if (check_cmd->parsed()) {
        Error err;
        auto clean = pm::vfs::sanitize_subpath(check_input, err);
        if (err) emit_error(err);
        if (check_write) clean = pm::vfs::sanitize_write_path(clean, check_dir);
        emit({{"ok", true}, {"path", clean}}, true);
    }

    if (quota_cmd->parsed()) {
        const auto cfg = pm::vfs::quota_config_from_env();
        nlohmann::json j = {
            {"ok", true},
            {"defaultMb", cfg.default_mb},
            {"defaultBytes", cfg.default_bytes},
            {"unlimited", cfg.unlimited},
        };
        if (quota_used >= 0 && quota_incoming >= 0) {
            const auto limit = quota_limit >= 0 ? static_cast<std::uint64_t>(quota_limit)
                                                 : cfg.default_bytes;
            j["exceed"] = pm::vfs::would_exceed_quota(
                static_cast<std::uint64_t>(quota_used),
                static_cast<std::uint64_t>(quota_incoming),
                limit,
                static_cast<std::uint64_t>(quota_replacing));
        }
        if (quota_home)
            j["homeTarget"] = pm::vfs::is_home_quota_target(quota_mount, quota_user, quota_bind);
        if (!quota_admin.empty())
            j["admin"] = pm::vfs::is_default_admin_id(quota_admin);
        emit(j, true);
    }

    if (mounts_cmd->parsed()) {
        auto store = load_store(config, binds);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& m : store.mounts()) arr.push_back({{"name", m.name}});
        emit({{"ok", true}, {"status", 200}, {"mounts", arr}}, true);
    }

    auto store = load_store(config, binds);

    if (serve_cmd->parsed()) return pm::vfs::serve_http(store, serve_host, serve_port);

    if (ls->parsed()) {
        auto listed = store.list(caller, ls_mount, ls_path, ls_glob);
        if (listed.error) emit_error(listed.error);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& n : listed.entries) arr.push_back(node_json(n));
        emit({{"ok", true}, {"status", 200}, {"entries", arr}}, true);
    }
    if (stat_cmd->parsed()) {
        auto st = store.stat(caller, stat_mount, stat_path);
        if (st.error) emit_error(st.error);
        auto j = node_json(st.node);
        j["ok"] = true;
        j["status"] = 200;
        emit(j, true);
    }
    if (read_cmd->parsed()) {
        auto rd = store.read(caller, read_mount, read_path, {});
        if (rd.error) emit_error(rd.error);
        emit({{"ok", true}, {"status", rd.status}, {"text", rd.body}, {"mime", rd.mime}}, true);
    }
    if (get_cmd->parsed()) {
        pm::vfs::Range range;
        if (!get_range.empty()) {
            // Size is applied inside read(); parse against a huge ceiling then re-parse is wrong.
            // Pass the text through by statting first.
            auto st = store.stat(caller, get_mount, get_path);
            const auto size = st.error ? 0 : st.node.size;
            range = pm::vfs::parse_range(get_range, size);
        }
        auto rd = store.read(caller, get_mount, get_path, range);
        nlohmann::json j = {
            {"ok", !rd.error},
            {"status", rd.status},
            {"mime", rd.mime},
            {"contentLength", rd.content_length},
            {"acceptRanges", rd.accept_ranges},
            {"body", rd.body},
        };
        if (!rd.content_range.empty()) j["contentRange"] = rd.content_range;
        if (rd.error) {
            j["error"] = rd.error.message;
            j["code"] = pm::vfs::canonical_vfs_errno(rd.error.code);
            emit(j, false);
        }
        emit(j, true);
    }
    if (write_cmd->parsed()) {
        std::string bytes = write_data;
        if (!write_file.empty()) {
            bytes = read_all(write_file);
            if (bytes.empty() && !std::filesystem::exists(write_file))
                emit_error(Error::make("ENOENT", "local file not found"));
        }
        auto wr = store.write(caller, write_mount, write_path, bytes);
        if (wr.error) emit_error(wr.error);
        auto j = node_json(wr.node);
        j["ok"] = true;
        j["status"] = 200;
        j["success"] = true;
        emit(j, true);
    }
    if (mkdir_cmd->parsed()) {
        auto made = store.mkdir(caller, mkdir_mount, mkdir_path);
        if (made.error) emit_error(made.error);
        auto j = node_json(made.node);
        j["ok"] = true;
        j["status"] = 200;
        j["success"] = true;
        emit(j, true);
    }
    if (rm_cmd->parsed()) {
        auto err = store.remove(caller, rm_mount, rm_path, rm_recursive);
        if (err) emit_error(err);
        emit({{"ok", true}, {"status", 200}, {"success", true}, {"path", rm_path}}, true);
    }
    if (copy_cmd->parsed()) {
        CopyRequest req;
        if (!copy_request.empty()) {
            try {
                req = request_from_json(nlohmann::json::parse(read_all(copy_request)));
            } catch (const std::exception& ex) {
                emit_error(Error::make("EINVAL", ex.what()));
            }
        } else {
            req.operation = copy_op;
            req.conflict_strategy = copy_strategy;
            req.conflict_mode = copy_mode;
            req.exclude_default = !no_exclude_default;
            req.include = includes;
            req.exclude = excludes;
            req.destination = Endpoint{dst_mount, dst_path};
            for (const auto& p : src_paths) req.sources.push_back(Endpoint{src_mount, p});
        }
        auto copied = store.copy(caller, req);
        nlohmann::json j = {
            {"ok", !copied.error},
            {"success", !copied.error},
            {"status", copied.error ? copied.error.status : 200},
            {"filesCopied", copied.report.files_copied},
            {"filesSkipped", copied.report.files_skipped},
            {"bytesCopied", copied.report.bytes_copied},
            {"destinationPath", copied.report.destination_path},
        };
        if (copied.report.conflict) {
            j["conflict"] = {
                {"source", copied.report.conflict->source},
                {"destination", copied.report.conflict->destination},
                {"suggestedPath", copied.report.conflict->suggested_path},
            };
        }
        if (copied.error) {
            j["error"] = copied.error.message;
            j["code"] = pm::vfs::canonical_vfs_errno(copied.error.code);
            emit(j, false);
        }
        emit(j, true);
    }
    return 2;
}
