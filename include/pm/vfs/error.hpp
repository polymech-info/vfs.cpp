#pragma once

#include <string>

namespace pm::vfs {

struct Error {
    std::string code;
    std::string message;
    int status = 0;

    explicit operator bool() const { return !code.empty(); }

    static Error ok();
    static Error make(std::string code, std::string message = {});
};

int vfs_errno_status(const std::string& code);
std::string canonical_vfs_errno(const std::string& code);
std::string default_vfs_message(const std::string& code);

/// `EDQUOT: …` or a bare `EACCES` token. Empty if nothing matches.
std::string guess_vfs_errno(const std::string& text);

struct HttpError {
    int status = 500;
    std::string error;
    std::string code;
};

HttpError map_vfs_errno(const std::string& code);

} // namespace pm::vfs
