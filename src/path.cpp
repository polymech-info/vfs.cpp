#include "pm/vfs/store.hpp"

#include "commons/path_sanitizer.hpp"

#include <algorithm>

namespace pm::vfs {
namespace {

Error error_from_sanitizer(const std::string& msg) {
    std::string code = "EFORBIDDEN";
    std::string text = msg;
    const auto colon = msg.find(':');
    if (colon != std::string::npos && colon > 0 && msg[0] == 'E') {
        code = msg.substr(0, colon);
        text = (colon + 1 < msg.size() && msg[colon + 1] == ' ') ? msg.substr(colon + 2) : msg.substr(colon + 1);
    }
    return Error::make(code, text);
}

bool glob_rec(std::string_view pat, std::size_t pi, std::string_view str, std::size_t si) {
    while (pi < pat.size()) {
        if (pat[pi] == '*' && pi + 1 < pat.size() && pat[pi + 1] == '*') {
            pi += 2;
            if (pi < pat.size() && pat[pi] == '/') ++pi;
            if (pi >= pat.size()) return true;
            for (std::size_t k = si; k <= str.size(); ++k) {
                if (k != si && (k == 0 || str[k - 1] != '/')) continue;
                if (glob_rec(pat, pi, str, k)) return true;
            }
            return false;
        }
        if (pat[pi] == '*') {
            ++pi;
            for (std::size_t k = si; k <= str.size(); ++k) {
                if (k > si && str[k - 1] == '/') break;
                if (glob_rec(pat, pi, str, k)) return true;
            }
            return false;
        }
        if (si >= str.size()) return false;
        if (pat[pi] == '?') {
            if (str[si] == '/') return false;
            ++pi;
            ++si;
            continue;
        }
        if (pat[pi] != str[si]) return false;
        ++pi;
        ++si;
    }
    return si == str.size();
}

} // namespace

bool match_glob(std::string_view pattern, std::string_view text) {
    return glob_rec(pattern, 0, text, 0);
}

std::string sanitize_subpath(std::string_view input, Error& err) {
    err = Error::ok();
    std::string msg;
    auto out = media::path::sanitize_subpath(std::string(input), msg);
    if (!msg.empty()) {
        err = error_from_sanitizer(msg);
        return {};
    }
    return out;
}

std::string sanitize_write_path(std::string_view subpath, bool is_directory) {
    std::string msg;
    return media::path::sanitize_write_path(std::string(subpath), msg, is_directory);
}

Range parse_range(std::string_view text, std::uint64_t size) {
    Range r;
    std::string s(text);
    if (s.rfind("bytes=", 0) == 0) s.erase(0, 6);
    if (s.empty()) return r;
    r.active = true;
    if (s[0] == '-') {
        r.suffix = true;
        try {
            const auto n = static_cast<std::uint64_t>(std::stoull(s.substr(1)));
            if (n == 0 || size == 0) {
                r.unsatisfiable = true;
                return r;
            }
            const auto take = std::min(n, size);
            r.start = size - take;
            r.end = size - 1;
        } catch (...) {
            r.unsatisfiable = true;
        }
        return r;
    }
    const auto dash = s.find('-');
    if (dash == std::string::npos) {
        r.unsatisfiable = true;
        return r;
    }
    try {
        r.start = static_cast<std::uint64_t>(std::stoull(s.substr(0, dash)));
        if (dash + 1 >= s.size()) {
            if (size == 0 || r.start >= size) {
                r.unsatisfiable = true;
                return r;
            }
            r.end = size - 1;
        } else {
            r.end = static_cast<std::uint64_t>(std::stoull(s.substr(dash + 1)));
        }
    } catch (...) {
        r.unsatisfiable = true;
        return r;
    }
    if (size == 0 || r.start >= size || r.end < r.start) {
        r.unsatisfiable = true;
        return r;
    }
    if (r.end >= size) r.end = size - 1;
    return r;
}

} // namespace pm::vfs
