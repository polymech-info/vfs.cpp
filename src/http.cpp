#include "pm/vfs/http.hpp"

#include "pm/vfs/tasks.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <memory>

namespace pm::vfs {
namespace {

nlohmann::json node_json(const Node& n) {
    return {
        {"name", n.name}, {"path", n.path}, {"size", n.size}, {"mtime", n.mtime},
        {"mime", n.mime}, {"parent", n.parent}, {"type", n.type},
    };
}

nlohmann::json task_json(const TaskView& task) {
    nlohmann::json j = {
        {"id", task.id},
        {"ownerId", task.owner_id},
        {"operation", task.operation},
        {"state", task.state},
        {"progress", task.progress},
        {"filesDone", task.files_done},
        {"totalFiles", task.total_files},
        {"bytesCopied", task.bytes_copied},
        {"seq", task.seq},
    };
    if (!task.error.empty()) j["error"] = task.error;
    if (task.has_conflict) {
        j["conflict"] = {
            {"source", task.conflict.source},
            {"destination", task.conflict.destination},
            {"suggestedPath", task.conflict.suggested_path},
        };
    }
    return j;
}

void send_json(httplib::Response& res, int status, const nlohmann::json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void send_error(httplib::Response& res, const Error& err) {
    const int status = err.status ? err.status : vfs_errno_status(err.code);
    send_json(res, status, {
        {"error", err.message.empty() ? default_vfs_message(err.code) : err.message},
        {"code", canonical_vfs_errno(err.code)},
    });
}

Caller caller_from(const httplib::Request& req) {
    Caller caller;
    if (req.has_header("X-Vfs-User")) caller.user_id = req.get_header_value("X-Vfs-User");
    return caller;
}

struct Located {
    std::string mount;
    std::string path;
};

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool split_mount(const std::string& path, const std::string& prefix, Located& loc) {
    if (!starts_with(path, prefix)) return false;
    const auto rest = path.substr(prefix.size());
    if (rest.empty()) return false;
    const auto slash = rest.find('/');
    if (slash == std::string::npos) {
        loc.mount = rest;
        loc.path.clear();
    } else {
        loc.mount = rest.substr(0, slash);
        loc.path = rest.substr(slash + 1);
    }
    return !loc.mount.empty();
}

bool task_id(const std::string& path, const std::string& suffix, std::string& id) {
    const std::string prefix = "/api/vfs/tasks/";
    if (!starts_with(path, prefix)) return false;
    auto rest = path.substr(prefix.size());
    if (!suffix.empty()) {
        if (rest.size() <= suffix.size() || rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) != 0)
            return false;
        rest.resize(rest.size() - suffix.size());
    }
    if (rest.empty() || rest.find('/') != std::string::npos) return false;
    id = rest;
    return true;
}

CopyRequest copy_from_json(const nlohmann::json& j) {
    CopyRequest req;
    req.operation = j.value("operation", std::string("copy"));
    req.conflict_mode = j.value("conflictMode", std::string("auto"));
    req.conflict_strategy = j.value("conflictStrategy", std::string("error"));
    req.exclude_default = j.value("excludeDefault", true);
    auto endpoint = [](const nlohmann::json& item) {
        Endpoint e;
        if (item.is_object()) {
            e.mount = item.value("mount", std::string{});
            e.path = item.value("path", std::string{});
        }
        return e;
    };
    if (j.contains("source")) req.source = endpoint(j["source"]);
    if (j.contains("destination")) req.destination = endpoint(j["destination"]);
    if (j.contains("sources") && j["sources"].is_array()) {
        for (const auto& item : j["sources"]) req.sources.push_back(endpoint(item));
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

bool terminal_state(const std::string& state) {
    return state == "completed" || state == "failed" || state == "cancelled";
}

} // namespace

int serve_http(const Store& store, const std::string& host, int port) {
    auto tasks = std::make_shared<TaskService>(store);
    httplib::Server server;

    auto list_at = [&store](const httplib::Request& req, httplib::Response& res, const Located& loc) {
        auto listed = store.list(caller_from(req), loc.mount, loc.path, req.get_param_value("glob"));
        if (listed.error) return send_error(res, listed.error);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& node : listed.entries) arr.push_back(node_json(node));
        send_json(res, 200, arr);
    };
    auto read_at = [&store](const httplib::Request& req, httplib::Response& res, const Located& loc, bool as_attachment) {
        auto rd = store.read(caller_from(req), loc.mount, loc.path, {});
        if (rd.error) return send_error(res, rd.error);
        // Leave status unset so httplib turns a Range header into 206.
        res.set_content(rd.body, as_attachment ? rd.mime : "text/plain");
        res.set_header("Accept-Ranges", "bytes");
    };
    auto write_at = [&store](const httplib::Request& req, httplib::Response& res, const Located& loc, std::string_view bytes) {
        auto wr = store.write(caller_from(req), loc.mount, loc.path, bytes);
        if (wr.error) return send_error(res, wr.error);
        send_json(res, 200, {{"success", true}, {"path", loc.path}, {"size", wr.node.size}});
    };

    // MSVC std::regex drops escapes and character classes, so routes are matched here.
    server.Get(R"(.*)", [&](const httplib::Request& req, httplib::Response& res) {
        Located loc;
        if (req.path == "/api/vfs/mounts") {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& mount : store.mounts()) arr.push_back({{"name", mount.name}});
            send_json(res, 200, arr);
            return;
        }
        if (split_mount(req.path, "/api/vfs/ls/", loc)) {
            list_at(req, res, loc);
            return;
        }
        if (split_mount(req.path, "/api/vfs/read/", loc)) return read_at(req, res, loc, false);
        if (split_mount(req.path, "/api/vfs/get/", loc)) return read_at(req, res, loc, true);
        if (split_mount(req.path, "/api/vfs/stat/", loc)) {
            auto st = store.stat(caller_from(req), loc.mount, loc.path);
            if (st.error) return send_error(res, st.error);
            send_json(res, 200, node_json(st.node));
            return;
        }
        std::string id;
        if (task_id(req.path, "/stream", id)) {
            TaskView existing;
            if (!tasks->get(id, existing)) return send_error(res, Error::make("ENOENT", "task not found"));
            res.set_chunked_content_provider(
                "application/x-ndjson",
                [tasks, id, seen = 0](size_t, httplib::DataSink& sink) mutable {
                    TaskView view;
                    if (!tasks->wait_event(id, seen, view, 15000)) {
                        sink.done();
                        return true;
                    }
                    seen = view.seq;
                    const auto line = task_json(view).dump() + "\n";
                    if (!sink.write(line.data(), line.size())) return false;
                    if (terminal_state(view.state)) sink.done();
                    return true;
                });
            return;
        }
        if (task_id(req.path, "", id)) {
            TaskView view;
            if (!tasks->get(id, view)) return send_error(res, Error::make("ENOENT", "task not found"));
            send_json(res, 200, task_json(view));
            return;
        }
        send_json(res, 404, {{"error", "Not found"}, {"code", "ENOENT"}});
    });

    server.Put(R"(.*)", [&](const httplib::Request& req, httplib::Response& res) {
        Located loc;
        if (!split_mount(req.path, "/api/vfs/write/", loc)) {
            send_json(res, 404, {{"error", "Not found"}, {"code", "ENOENT"}});
            return;
        }
        write_at(req, res, loc, req.body);
    });

    server.Post(R"(.*)", [&](const httplib::Request& req, httplib::Response& res) {
        Located loc;
        if (split_mount(req.path, "/api/vfs/upload/", loc)) {
            if (!req.has_file("file")) {
                send_json(res, 400, {{"error", "No file provided"}, {"code", "EINVAL"}});
                return;
            }
            write_at(req, res, loc, req.get_file_value("file").content);
            return;
        }
        if (split_mount(req.path, "/api/vfs/mkdir/", loc)) {
            auto made = store.mkdir(caller_from(req), loc.mount, loc.path);
            if (made.error) return send_error(res, made.error);
            send_json(res, 200, {{"success", true}, {"path", loc.path}});
            return;
        }
        auto parse_body = [&](nlohmann::json& body) {
            try {
                body = nlohmann::json::parse(req.body);
                return true;
            } catch (const std::exception& ex) {
                send_json(res, 400, {{"error", ex.what()}, {"code", "EINVAL"}});
                return false;
            }
        };
        if (req.path == "/api/vfs/copy" || req.path == "/api/vfs/transfer") {
            nlohmann::json body;
            if (!parse_body(body)) return;
            if (req.path == "/api/vfs/transfer") {
                send_json(res, 200, {{"taskId", tasks->start(caller_from(req), copy_from_json(body))}});
                return;
            }
            auto copied = store.copy(caller_from(req), copy_from_json(body));
            nlohmann::json j = {
                {"success", !copied.error},
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
                j["code"] = canonical_vfs_errno(copied.error.code);
                send_json(res, copied.error.status ? copied.error.status : 500, j);
                return;
            }
            send_json(res, 200, j);
            return;
        }
        std::string id;
        if (task_id(req.path, "/resolve", id)) {
            nlohmann::json body;
            if (!parse_body(body)) return;
            auto err = tasks->resolve(id, body.value("decision", std::string{}));
            if (err) return send_error(res, err);
            send_json(res, 200, {{"success", true}});
            return;
        }
        send_json(res, 404, {{"error", "Not found"}, {"code", "ENOENT"}});
    });

    server.Delete(R"(.*)", [&](const httplib::Request& req, httplib::Response& res) {
        Located loc;
        if (!split_mount(req.path, "/api/vfs/delete/", loc)) {
            send_json(res, 404, {{"error", "Not found"}, {"code", "ENOENT"}});
            return;
        }
        auto err = store.remove(caller_from(req), loc.mount, loc.path, true);
        if (err) return send_error(res, err);
        send_json(res, 200, {{"success", true}, {"path", loc.path}});
    });

    int bound = port;
    if (port == 0) {
        bound = server.bind_to_any_port(host);
    } else if (!server.bind_to_port(host, port)) {
        bound = -1;
    }
    if (bound < 0) {
        std::cout << nlohmann::json{{"ok", false}, {"error", "bind failed"}, {"code", "EADDRINUSE"}}.dump()
                  << std::endl;
        return 1;
    }
    std::cout << nlohmann::json{{"ok", true}, {"host", host}, {"port", bound}}.dump() << std::endl;
    return server.listen_after_bind() ? 0 : 1;
}

} // namespace pm::vfs
