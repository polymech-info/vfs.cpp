#pragma once

#include "pm/vfs/store.hpp"

#include <memory>
#include <string>

namespace pm::vfs {

struct TaskView {
    std::string id;
    std::string owner_id;
    std::string operation;
    std::string state;
    int progress = 0;
    int files_done = 0;
    int total_files = 0;
    std::uint64_t bytes_copied = 0;
    int seq = 0;
    std::string error;
    bool has_conflict = false;
    Conflict conflict;
};

class TaskService : public std::enable_shared_from_this<TaskService> {
public:
    explicit TaskService(Store store);

    std::string start(const Caller& caller, CopyRequest request);
    bool get(const std::string& id, TaskView& out) const;
    Error resolve(const std::string& id, const std::string& decision);
    /// Blocks until `seq` moves past `after`, or the task is already terminal
    /// and has nothing newer. False on timeout or unknown id.
    bool wait_event(const std::string& id, int after, TaskView& out, int timeout_ms);

private:
    struct Task;
    std::string await_decision(const std::string& id, const Conflict& conflict);
    void finish(const std::string& id, const CopyResult& result);

    Store store_;
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pm::vfs
