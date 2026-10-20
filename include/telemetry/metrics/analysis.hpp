#pragma once

#include "telemetry/simulation/simulation.hpp"
#include <deque>
#include <string>

namespace telemetry::metrics {
struct DurationMetric {
    std::optional<SimDuration> value;
    std::string reason;
    bool operator==(const DurationMetric&) const = default;
};
struct AttemptReport {
    AssignmentCommand assignment;
    SimTime assigned_at;
    bool physical_assigned = false;
    std::optional<SimTime> started_at,finished_at,interrupted_at,revoked_at,accepted_at;
    std::optional<core::ResultDecision> result;
    bool operator==(const AttemptReport&) const = default;
};
struct TaskReport {
    TaskDescription description;
    std::optional<SimTime> completed_at;
    std::optional<AttemptNo> current;
    std::uint64_t attempts = 0;
    bool operator==(const TaskReport&) const = default;
};
struct NodeReport {
    NodeId id;
    std::optional<SimTime> crash_at,failed_at;
    core::NodeStatus status = core::NodeStatus::alive;
    std::string classification;
    std::vector<TaskId> returned;
    DurationMetric detection,recovery;
    bool operator==(const NodeReport&) const = default;
};
struct Throughput {
    simulation::MeasurementWindow window;
    std::uint64_t completed;
    long double tasks_per_second;
    bool operator==(const Throughput&) const = default;
};
struct Summary {
    std::uint64_t arrived = 0,completed = 0,unfinished = 0,queued = 0,assigned = 0;
    std::uint64_t retries = 0,rejected_revoked_results = 0,crashes = 0,false_positives = 0,false_negatives = 0;
    std::uint64_t crashes_after_false_exclusion = 0,detection_samples = 0,recovery_samples = 0,incomplete_recoveries = 0;
    std::optional<long double> mean_latency_ticks,mean_detection_ticks,mean_recovery_ticks;
    std::optional<SimDuration> p95_latency;
    std::vector<Throughput> throughput;
    bool operator==(const Summary&) const = default;
};
struct Analysis {
    Summary summary;
    std::map<TaskId,TaskReport> tasks;
    std::map<simulation::PhysicalAttemptKey,AttemptReport> attempts;
    std::map<NodeId,NodeReport> nodes;
    bool operator==(const Analysis&) const = default;
};
// Nearest rank ceil(0.95*n), without multiplying n. Empty input has no rank.
[[nodiscard]] std::optional<std::size_t> p95_rank(std::size_t count) noexcept;
class Analyzer final : public simulation::SimulationAuditSink {
public:
    explicit Analyzer(std::shared_ptr<const simulation::Scenario> scenario);
    void on_event(const engine::EventKey& key,const simulation::SimulationEvent& event) override;
    // Called only after a successful run or a validated journal completion marker.
    [[nodiscard]] Analysis finish(engine::RunResult completed);
private:
    void logical(const core::AuditEvent& event);
    void physical(const simulation::PhysicalEvent& event);
    void transition(NodeId id,core::NodeStatus before,core::NodeStatus after,SimTime at);
    AttemptReport& attempt(ResultMessage message);
    void revoke(SimTime at,ResultMessage message);
    std::shared_ptr<const simulation::Scenario> scenario_;
    Analysis report_;
    std::deque<TaskId> pending_;
    std::map<NodeId,std::deque<simulation::PhysicalAttemptKey>> waiting_;
    std::map<NodeId,std::optional<simulation::PhysicalAttemptKey>> running_;
    std::set<NodeId> crashed_,pending_failed_,configured_failed_;
    std::set<TaskId> arrived_;
    std::optional<engine::EventKey> last_;
    std::optional<SimTime> last_reconfiguration_,last_dispatch_;
    std::uint64_t actions_ = 0,commands_ = 0;
    bool finished_ = false,failed_ = false;
};
}  // namespace telemetry::metrics
