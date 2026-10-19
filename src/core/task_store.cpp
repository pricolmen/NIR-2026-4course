#include "telemetry/core/task_store.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace telemetry::core {
void TaskStore::check_time(SimTime at) const {
    if (notifying_) { throw std::logic_error("Audit callbacks must not mutate TaskStore"); }
    if (last_observation_ && at < *last_observation_) {
        throw std::invalid_argument("TaskStore observation time moved backwards");
    }
}

void TaskStore::emit(const AuditEvent& event) {
    if (!sink_) { return; }
    notifying_ = true;
    try { sink_->on_event(event); }
    catch (...) { notifying_ = false; throw; }
    notifying_ = false;
}

void TaskStore::arrive(SimTime at, TaskDescription task) {
    check_time(at);
    if (task.arrival != at) { throw std::invalid_argument("Arrival observation must match original arrival time"); }
    if (!std::isfinite(task.work) || task.work <= 0) {
        throw std::invalid_argument("Task work must be finite and positive");
    }
    if (tasks_.contains(task.id)) { throw std::invalid_argument("Duplicate task ID"); }
    pending_.push_back(task.id);
    try { tasks_.emplace(task.id, TaskRecord{task, {}, std::nullopt, std::nullopt}); }
    catch (...) { pending_.pop_back(); throw; }
    last_observation_ = at;
    emit(TaskArrivedEvent{at, task});
}

std::optional<AssignmentCommand> TaskStore::assign_next(SimTime at, NodeId node) {
    check_time(at);
    if (pending_.empty()) { last_observation_ = at; return std::nullopt; }
    auto& task = tasks_.at(pending_.front());
    const auto number = task.attempts.empty() ? AttemptNo{1} : task.attempts.back().number.next();
    const AssignmentCommand command{task.description, node, number};
    task.attempts.push_back({number, node, at, std::nullopt, std::nullopt});
    task.current_attempt = number;
    pending_.pop_front();
    last_observation_ = at;
    emit(TaskAssignedEvent{at, command});
    return command;
}

ResultDecision TaskStore::inspect_result(ResultMessage result) const {
    const auto found = tasks_.find(result.task);
    if (found == tasks_.end()) { return ResultDecision::unknown_task; }
    const auto& task = found->second;
    if (task.completed_at) { return ResultDecision::already_completed; }
    if (!task.current_attempt) { return ResultDecision::no_current_assignment; }
    if (result.attempt != *task.current_attempt) { return ResultDecision::attempt_mismatch; }
    if (result.node != task.attempts.back().node) { return ResultDecision::node_mismatch; }
    return ResultDecision::accepted;
}

ResultDecision TaskStore::receive_result(SimTime at, ResultMessage result) {
    check_time(at);
    const auto decision = inspect_result(result);
    if (decision == ResultDecision::accepted) {
        auto& task = tasks_.at(result.task);
        // No allocations or observer callbacks between these updates.
        task.attempts.back().accepted_at = at;
        task.completed_at = at;
        task.current_attempt.reset();
    }
    last_observation_ = at;
    emit(ResultEvent{at, result, decision});
    return decision;
}

bool TaskStore::revoke_assignment(SimTime at, TaskId id) {
    check_time(at);
    auto& task = tasks_.at(id);
    if (!task.current_attempt) { last_observation_ = at; return false; }
    const ResultMessage assignment{id, *task.current_attempt, task.attempts.back().node};
    // Allocate before changing the current attempt so allocation failure leaves
    // the logical assignment intact.
    pending_.push_back(id);
    task.attempts.back().revoked_at = at;
    task.current_attempt.reset();
    last_observation_ = at;
    emit(AssignmentRevokedEvent{at, assignment});
    return true;
}

TaskStore::PreparedRevocations TaskStore::prepare_revocations(
    SimTime at, std::span<const NodeId> sorted_nodes) {
    check_time(at);
    PreparedRevocations prepared;
    if (sorted_nodes.empty()) { return prepared; }
    for (auto& [id, task] : tasks_) {
        static_cast<void>(id);
        if (task.current_attempt &&
            std::binary_search(sorted_nodes.begin(), sorted_nodes.end(), task.attempts.back().node)) {
            prepared.tasks.push_back(&task);
        }
    }
    std::sort(prepared.tasks.begin(), prepared.tasks.end(), [](const auto* a, const auto* b) {
        if (a->description.arrival != b->description.arrival) {
            return a->description.arrival < b->description.arrival;
        }
        return a->description.id < b->description.id;
    });
    if (prepared.tasks.empty()) { return prepared; }
    prepared.pending = pending_;
    for (const auto* task : prepared.tasks) { prepared.pending.push_back(task->description.id); }
    return prepared;
}

void TaskStore::commit_revocations(SimTime at, PreparedRevocations& prepared) noexcept {
    for (auto* task : prepared.tasks) {
        task->attempts.back().revoked_at = at;
        task->current_attempt.reset();
    }
    if (!prepared.tasks.empty()) { pending_.swap(prepared.pending); }
    last_observation_ = at;
    // No callbacks until Dispatcher has committed all parts of the batch.
}

TaskCounts TaskStore::counts() const noexcept {
    TaskCounts result{tasks_.size(), 0, pending_.size(), 0};
    for (const auto& [id, task] : tasks_) {
        static_cast<void>(id);
        if (task.completed_at) { ++result.completed; }
        if (task.current_attempt) { ++result.assigned; }
    }
    return result;
}

void TaskStore::validate_invariants() const {
    const auto require = [](bool condition) {
        if (!condition) { throw std::logic_error("TaskStore invariant violated"); }
    };
    std::set<TaskId> queued;
    for (const auto id : pending_) {
        require(tasks_.contains(id));
        require(queued.insert(id).second);
    }
    for (const auto& [id, task] : tasks_) {
        require(task.description.id == id);
        require(std::isfinite(task.description.work) && task.description.work > 0);
        std::size_t active = 0;
        std::size_t accepted = 0;
        auto expected = AttemptNo{1};
        for (std::size_t index = 0; index < task.attempts.size(); ++index) {
            const auto& attempt = task.attempts[index];
            require(attempt.number == expected);
            require(attempt.assigned_at >= task.description.arrival);
            if (index > 0) {
                const auto& previous = task.attempts[index - 1];
                require(previous.revoked_at.has_value());
                require(attempt.assigned_at >= *previous.revoked_at);
            }
            require(!(attempt.revoked_at && attempt.accepted_at));
            if (attempt.revoked_at) { require(*attempt.revoked_at >= attempt.assigned_at); }
            if (attempt.accepted_at) {
                ++accepted;
                require(*attempt.accepted_at >= attempt.assigned_at);
                require(task.completed_at == attempt.accepted_at);
                require(index == task.attempts.size() - 1);
            }
            if (!attempt.revoked_at && !attempt.accepted_at) {
                ++active;
                require(index == task.attempts.size() - 1);
                require(task.current_attempt == attempt.number);
            }
            if (index + 1 < task.attempts.size()) { expected = expected.next(); }
        }
        require(active == (task.current_attempt ? 1U : 0U));
        require(accepted == (task.completed_at ? 1U : 0U));
        if (task.completed_at) { require(!task.current_attempt && !queued.contains(id)); }
        else if (task.current_attempt) { require(!queued.contains(id)); }
        else { require(queued.contains(id)); }
    }
    const auto balance = counts();
    require(balance.arrived == balance.completed + balance.queued + balance.assigned);
}
}  // namespace telemetry::core
