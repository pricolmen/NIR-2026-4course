#pragma once

#include "telemetry/core/audit.hpp"
#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace telemetry::core {
struct AttemptRecord {
    AttemptNo number;
    NodeId node;
    SimTime assigned_at;
    std::optional<SimTime> revoked_at;
    std::optional<SimTime> accepted_at;
    bool operator==(const AttemptRecord&) const = default;
};

struct TaskRecord {
    TaskDescription description;
    std::vector<AttemptRecord> attempts;
    std::optional<AttemptNo> current_attempt;
    std::optional<SimTime> completed_at;
    bool operator==(const TaskRecord&) const = default;
};

struct TaskCounts {
    std::size_t arrived = 0;
    std::size_t completed = 0;
    std::size_t queued = 0;
    std::size_t assigned = 0;
    bool operator==(const TaskCounts&) const = default;
};

class TaskStore {
public:
    // The optional sink must outlive this store. No journal is buffered here.
    explicit TaskStore(AuditSink* sink = nullptr) : sink_(sink) {}
    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;

    void arrive(SimTime at, TaskDescription task);
    [[nodiscard]] std::optional<AssignmentCommand> assign_next(SimTime at, NodeId node);
    [[nodiscard]] ResultDecision receive_result(SimTime at, ResultMessage result);
    [[nodiscard]] ResultDecision inspect_result(ResultMessage result) const;
    // Single-assignment primitive, appending to FIFO; use Dispatcher for
    // joint FAILED handling. Neither operation cancels physical work.
    [[nodiscard]] bool revoke_assignment(SimTime at, TaskId task);

    [[nodiscard]] const std::map<TaskId, TaskRecord>& tasks() const noexcept { return tasks_; }
    [[nodiscard]] const std::deque<TaskId>& pending() const noexcept { return pending_; }
    [[nodiscard]] TaskCounts counts() const noexcept;
    void validate_invariants() const;

private:
    // Dispatcher coordinates the prepared store change with availability/load.
    // This plan is private, cannot outlive an intervening store mutation, and
    // is committed immediately while Dispatcher rejects reentrant mutations.
    friend class Dispatcher;
    struct PreparedRevocations {
        std::deque<TaskId> pending;
        std::vector<TaskRecord*> tasks;
    };
    [[nodiscard]] PreparedRevocations prepare_revocations(SimTime at, std::span<const NodeId> sorted_nodes);
    void commit_revocations(SimTime at, PreparedRevocations& prepared) noexcept;
    void check_time(SimTime at) const;
    void emit(const AuditEvent& event);
    std::map<TaskId, TaskRecord> tasks_;
    std::deque<TaskId> pending_;
    std::optional<SimTime> last_observation_;
    AuditSink* sink_;
    bool notifying_ = false;
};
}  // namespace telemetry::core
