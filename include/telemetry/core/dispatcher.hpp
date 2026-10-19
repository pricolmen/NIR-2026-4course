#pragma once

#include "telemetry/core/scheduling.hpp"
#include "telemetry/core/task_store.hpp"

namespace telemetry::core {
enum class SchedulingPolicy { round_robin, least_loaded };

class Dispatcher final : private AuditSink {
public:
    Dispatcher(std::vector<NodeDefinition> nodes, SchedulingPolicy policy, AuditSink* sink = nullptr);
    void arrive(SimTime at, TaskDescription task);
    // One final set for all availability changes at a timestamp, not individual
    // intermediate changes. Only the caller's logical ALIVE set is supplied.
    void set_alive(SimTime at, std::vector<NodeId> alive);
    // One final ALIVE set and all FAILED notifications of this timestamp.
    // Known FAILED notifications are idempotent; FAILED can never rejoin ALIVE.
    // Returns logical revocations only. Call dispatch separately after arrivals.
    [[nodiscard]] ReconfigurationEvent apply_availability(
        SimTime at, std::vector<NodeId> alive, std::vector<NodeId> failed);
    [[nodiscard]] std::span<const NodeId> failed() const noexcept { return failed_; }
    [[nodiscard]] std::vector<AssignmentCommand> dispatch(SimTime at);
    [[nodiscard]] ResultDecision receive_result(SimTime at, ResultMessage result);
    [[nodiscard]] bool revoke_assignment(SimTime at, TaskId task);
    [[nodiscard]] const TaskStore& task_store() const noexcept { return tasks_; }
    [[nodiscard]] const LoadLedger& loads() const noexcept { return loads_; }
    [[nodiscard]] std::span<const NodeId> alive() const noexcept { return alive_; }
    void validate_invariants() const;
private:
    void on_event(const AuditEvent& event) override;
    void check_time(SimTime at) const;
    LoadLedger loads_;
    std::vector<NodeId> alive_;
    std::vector<NodeId> failed_;
    RoundRobin round_robin_;
    TaskStore tasks_;
    SchedulingPolicy policy_;
    AuditSink* sink_;
    std::optional<SimTime> last_observation_;
    bool operating_ = false;
};
}  // namespace telemetry::core
