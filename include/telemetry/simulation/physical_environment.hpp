#pragma once

#include "telemetry/simulation/physical_trace.hpp"
#include "telemetry/simulation/phases.hpp"
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <variant>

namespace telemetry::simulation {
struct PhysicalAttemptKey {
    TaskId task;
    AttemptNo attempt;
    auto operator<=>(const PhysicalAttemptKey&) const = default;
};
struct PhysicalAttempt {
    AssignmentCommand assignment;
    SimTime assigned_at;
    SimDuration duration;
    std::optional<SimTime> started_at;
    std::optional<SimTime> finished_at;
    std::optional<SimTime> interrupted_at;
    std::optional<SimTime> completion_due; // Only deadlines within horizon.
    std::optional<std::uint64_t> completion_token;
};
struct PhysicalNode {
    NodeDefinition definition;
    bool crashed = false;
    std::optional<PhysicalAttemptKey> running;
    std::deque<PhysicalAttemptKey> waiting;
    std::uint64_t completion_token = 0; // Independent of attempts/detector generations.
    std::uint64_t sent_heartbeats = 0;
};
struct PhysicalAssigned { SimTime at; AssignmentCommand assignment; };
struct PhysicalStarted {
    SimTime at;
    ResultMessage attempt;
    SimDuration duration;
    std::optional<SimTime> completion_due;
    std::uint64_t token;
};
struct PhysicalFinished { SimTime at; ResultMessage attempt; };
struct PhysicalCrash { SimTime at; NodeId node; std::optional<ResultMessage> interrupted; };
enum class CompletionSkipReason { crashed, stale_token };
struct PhysicalCompletionSkipped {
    SimTime at;
    ResultMessage attempt;
    std::uint64_t scheduled_token;
    std::uint64_t current_token;
    CompletionSkipReason reason;
};
enum class DeliveryDisposition { scheduled, trace_lost, blackout, beyond_horizon };
struct PhysicalHeartbeatSent {
    SimTime at;
    HeartbeatMessage message;
    SimDuration delay;
    DeliveryDisposition disposition;
    std::optional<SimTime> receive_at; // Planned receipt within horizon, even if lost.
};
struct PhysicalHeartbeatDelivered { SimTime at; SimTime sent_at; HeartbeatMessage message; };
struct PhysicalSendSkipped { SimTime at; HeartbeatMessage message; };
using PhysicalEvent = std::variant<PhysicalAssigned,PhysicalStarted,PhysicalFinished,PhysicalCrash,
    PhysicalCompletionSkipped,PhysicalHeartbeatSent,PhysicalHeartbeatDelivered,PhysicalSendSkipped>;
class PhysicalAuditSink {
public:
    virtual ~PhysicalAuditSink() = default;
    virtual void on_event(const PhysicalEvent& event) = 0;
};
class ObservationSink {
public:
    virtual ~ObservationSink() = default;
    virtual void on_result(SimTime at, ResultMessage result) = 0;
    virtual void on_heartbeat(SimTime at, HeartbeatMessage message) = 0;
};

class PhysicalEnvironment {
public:
    // Environment and sinks must outlive execution of callbacks in the loop.
    PhysicalEnvironment(engine::EventLoop& loop, std::shared_ptr<const PhysicalTrace> trace,
                        ObservationSink& observations, PhysicalAuditSink* audit = nullptr);
    PhysicalEnvironment(const PhysicalEnvironment&) = delete;
    PhysicalEnvironment& operator=(const PhysicalEnvironment&) = delete;
    void initialize(); // Once, before run: crash events and the first sends.
    void submit(AssignmentCommand command); // Only in dispatch phase 8.
    [[nodiscard]] const std::map<NodeId,PhysicalNode>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::map<PhysicalAttemptKey,PhysicalAttempt>& attempts() const noexcept { return attempts_; }
    void validate_invariants() const;
private:
    void require_phase(engine::Phase phase) const;
    void emit(const PhysicalEvent& event);
    [[nodiscard]] std::optional<PhysicalStarted> start_next(PhysicalNode& node);
    void crash(NodeId node);
    void complete(NodeId node, PhysicalAttemptKey key, std::uint64_t token);
    void send(NodeId node, HeartbeatSeq sequence);
    void deliver(SimTime sent_at, HeartbeatMessage message);
    engine::EventLoop& loop_;
    std::shared_ptr<const PhysicalTrace> trace_;
    ObservationSink& observations_;
    PhysicalAuditSink* audit_;
    std::map<NodeId,PhysicalNode> nodes_;
    std::map<PhysicalAttemptKey,PhysicalAttempt> attempts_;
    bool initialization_attempted_ = false;
    bool initialized_ = false;
    bool operating_ = false;
};
}  // namespace telemetry::simulation
