#include "pm/vfs/tasks.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace pm::vfs {
namespace {

std::string make_id() {
    static std::atomic<unsigned long long> n{1};
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", n.fetch_add(1));
    return buf;
}

bool terminal_state(const std::string& state) {
    return state == "completed" || state == "failed" || state == "cancelled";
}

} // namespace

struct TaskService::Task {
    std::string id;
    std::string owner_id;
    std::string operation = "copy";
    std::string state = "running";
    int progress = 0;
    int files_done = 0;
    int total_files = 0;
    std::uint64_t bytes_copied = 0;
    int seq = 1;
    std::string error;
    bool has_conflict = false;
    Conflict conflict;
    bool decision_ready = false;
    std::string decision;
};

struct TaskService::State {
    mutable std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<std::string, Task> tasks;
};

TaskService::TaskService(Store store)
    : store_(std::move(store)), state_(std::make_shared<State>()) {}

std::string TaskService::await_decision(const std::string& id, const Conflict& conflict) {
    std::unique_lock lock(state_->mu);
    auto it = state_->tasks.find(id);
    if (it == state_->tasks.end()) return "cancel";
    it->second.state = "paused_for_conflict";
    it->second.has_conflict = true;
    it->second.conflict = conflict;
    it->second.decision_ready = false;
    it->second.decision.clear();
    ++it->second.seq;
    state_->cv.notify_all();
    state_->cv.wait(lock, [&] {
        auto cur = state_->tasks.find(id);
        return cur != state_->tasks.end() && cur->second.decision_ready;
    });
    it = state_->tasks.find(id);
    if (it == state_->tasks.end()) return "cancel";
    const auto decision = it->second.decision.empty() ? std::string("cancel") : it->second.decision;
    it->second.state = "running";
    it->second.has_conflict = false;
    ++it->second.seq;
    state_->cv.notify_all();
    return decision;
}

void TaskService::finish(const std::string& id, const CopyResult& result) {
    std::lock_guard lock(state_->mu);
    auto it = state_->tasks.find(id);
    if (it == state_->tasks.end()) return;
    auto& task = it->second;
    task.files_done = result.report.files_copied + result.report.files_skipped;
    task.total_files = task.files_done;
    task.bytes_copied = result.report.bytes_copied;
    task.has_conflict = false;
    if (!result.error) {
        task.state = "completed";
        task.progress = 100;
    } else if (result.error.code == "ECANCELED") {
        task.state = "cancelled";
        task.error = result.error.message;
    } else {
        task.state = "failed";
        task.error = result.error.message;
        if (result.report.conflict) {
            task.has_conflict = true;
            task.conflict = *result.report.conflict;
        }
    }
    ++task.seq;
    state_->cv.notify_all();
}

std::string TaskService::start(const Caller& caller, CopyRequest request) {
    Task task;
    task.id = make_id();
    task.owner_id = caller.user_id.empty() ? "anonymous" : caller.user_id;
    if (request.operation == "move" || request.operation == "delete" || request.operation == "copy")
        task.operation = request.operation;
    {
        std::lock_guard lock(state_->mu);
        state_->tasks.emplace(task.id, task);
    }

    const auto id = task.id;
    auto self = shared_from_this();
    std::string sticky;
    if (request.conflict_mode == "manual") {
        request.on_conflict = [self, id, sticky](const Conflict& conflict) mutable {
            if (!sticky.empty()) return sticky;
            auto decision = self->await_decision(id, conflict);
            if (decision == "overwrite_all") {
                sticky = "overwrite";
                return sticky;
            }
            if (decision == "skip_all") {
                sticky = "skip";
                return sticky;
            }
            return decision;
        };
    }

    std::thread([self, id, caller, request = std::move(request)]() mutable {
        const auto result = self->store_.copy(caller, request);
        self->finish(id, result);
    }).detach();
    return id;
}

bool TaskService::get(const std::string& id, TaskView& out) const {
    std::lock_guard lock(state_->mu);
    auto it = state_->tasks.find(id);
    if (it == state_->tasks.end()) return false;
    const auto& task = it->second;
    out.id = task.id;
    out.owner_id = task.owner_id;
    out.operation = task.operation;
    out.state = task.state;
    out.progress = task.progress;
    out.files_done = task.files_done;
    out.total_files = task.total_files;
    out.bytes_copied = task.bytes_copied;
    out.seq = task.seq;
    out.error = task.error;
    out.has_conflict = task.has_conflict;
    out.conflict = task.conflict;
    return true;
}

Error TaskService::resolve(const std::string& id, const std::string& decision) {
    std::lock_guard lock(state_->mu);
    auto it = state_->tasks.find(id);
    if (it == state_->tasks.end()) return Error::make("ENOENT", "task not found");
    auto& task = it->second;
    if (task.state != "paused_for_conflict")
        return Error::make("EINVAL", "task is not paused for a conflict");
    task.decision = decision;
    task.decision_ready = true;
    state_->cv.notify_all();
    return Error::ok();
}

bool TaskService::wait_event(const std::string& id, int after, TaskView& out, int timeout_ms) {
    std::unique_lock lock(state_->mu);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto ready = [&] {
        auto it = state_->tasks.find(id);
        if (it == state_->tasks.end()) return true;
        return it->second.seq > after;
    };
    if (!state_->cv.wait_until(lock, deadline, ready)) return false;
    auto it = state_->tasks.find(id);
    if (it == state_->tasks.end()) return false;
    if (it->second.seq <= after) return false;
    lock.unlock();
    return get(id, out);
}

} // namespace pm::vfs
