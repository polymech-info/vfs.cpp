#include "pm/vfs/error.hpp"

#include <cctype>

namespace pm::vfs {

Error Error::ok() { return {}; }

Error Error::make(std::string code, std::string message) {
    Error e;
    e.code = std::move(code);
    e.status = vfs_errno_status(e.code);
    e.message = message.empty() ? default_vfs_message(e.code) : std::move(message);
    return e;
}

int vfs_errno_status(const std::string& code) {
    if (code == "ENOENT" || code == "ENOTDIR") return 404;
    if (code == "EACCES" || code == "EACCESS" || code == "EFORBIDDEN") return 403;
    if (code == "EISDIR" || code == "EINVAL" || code == "ELOOP" || code == "EMLINK") return 400;
    if (code == "EEXIST") return 409;
    if (code == "EFBIG") return 413;
    if (code == "ENOSPC" || code == "EDQUOT") return 507;
    if (code == "auth_required") return 401;
    return 500;
}

std::string canonical_vfs_errno(const std::string& code) {
    if (code == "EACCESS" || code == "EFORBIDDEN") return "EACCES";
    if (code.empty()) return "EINTERNAL";
    return code;
}

std::string default_vfs_message(const std::string& code) {
    if (code == "ENOENT" || code == "ENOTDIR") return "Not found";
    if (code == "EISDIR") return "Path is a directory";
    if (code == "EACCES" || code == "EACCESS" || code == "EFORBIDDEN") return "Forbidden";
    if (code == "EEXIST") return "Conflict";
    if (code == "EINVAL") return "Bad request";
    if (code == "ENOSPC") return "Insufficient Storage";
    if (code == "EDQUOT") return "Quota exceeded";
    if (code == "EFBIG") return "Payload Too Large";
    if (code == "ELOOP") return "Bind mount cycle detected";
    if (code == "EMLINK") return "Maximum bind mount depth exceeded";
    if (code == "auth_required") return "home mount requires authentication";
    return "Internal Server Error";
}

std::string guess_vfs_errno(const std::string& text) {
    std::string token;
    auto flush = [&](std::string& out) {
        if (out.size() >= 3 && out[0] == 'E') {
            bool letters = true;
            for (std::size_t i = 1; i < out.size(); ++i) {
                if (!std::isupper(static_cast<unsigned char>(out[i]))) {
                    letters = false;
                    break;
                }
            }
            if (letters) token = out;
        }
        out.clear();
    };
    std::string cur;
    for (char c : text) {
        if (std::isupper(static_cast<unsigned char>(c)) || (c == 'E' && cur.empty()))
            cur.push_back(c);
        else
            flush(cur);
    }
    flush(cur);
    return token;
}

HttpError map_vfs_errno(const std::string& code) {
    HttpError out;
    out.status = vfs_errno_status(code);
    out.code = canonical_vfs_errno(code);
    out.error = default_vfs_message(code.empty() ? out.code : code);
    if (out.error == "Internal Server Error" && code != out.code)
        out.error = default_vfs_message(out.code);
    return out;
}

} // namespace pm::vfs
