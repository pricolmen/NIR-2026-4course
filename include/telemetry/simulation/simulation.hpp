#pragma once

#include "telemetry/core/dispatcher.hpp"
#include "telemetry/simulation/physical_environment.hpp"
#include "telemetry/simulation/scenario.hpp"
#include <limits>
#include <set>

namespace telemetry::simulation {
enum class Variant { fixed_rr, fixed_ll, adaptive_rr, adaptive_ll };
enum class SimulationState { ready, running, completed, failed };
struct HeartbeatObserved {
    SimTime at;
    HeartbeatMessage message;
    core::HeartbeatUpdate update;
    std::uint64_t generation;
    SimTime contact;
    core::DetectorThresholds thresholds;
};
struct TimeoutObserved {
    SimTime at;
    core::TimeoutRequest request;
    core::TimeoutUpdate update;
    core::DetectorThresholds thresholds;
};
struct DispatchBatch { SimTime at; std::size_t commands; };
using SimulationEvent = std::variant<core::AuditEvent,PhysicalEvent,HeartbeatObserved,TimeoutObserved,DispatchBatch>;
class SimulationAuditSink {
public:
    virtual ~SimulationAuditSink() = default;
    // Synchronous event/key references live only during this call. Errors abort
    // the run without rolling back already committed logical/physical state.
    virtual void on_event(const engine::EventKey& key,const SimulationEvent& event) = 0;
};

class Simulation final : private ObservationSink, private core::AuditSink, private PhysicalAuditSink {
public:
    // Scenario is shared immutable input; each instance owns all mutable state.
    // The optional sink must outlive run(). max_events is an unsigned limit.
    Simulation(std::shared_ptr<const Scenario> scenario,Variant variant,SimulationAuditSink* sink = nullptr,
               std::uint64_t max_events = std::numeric_limits<std::uint64_t>::max());
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;
    [[nodiscard]] engine::RunResult run();
    [[nodiscard]] SimulationState state() const noexcept { return state_; }
    [[nodiscard]] const engine::EventLoop& clock() const noexcept { return loop_; }
    [[nodiscard]] const core::Dispatcher& dispatcher() const noexcept { return dispatcher_; }
    [[nodiscard]] const std::map<NodeId,core::Detector>& detectors() const noexcept { return detectors_; }
    [[nodiscard]] const PhysicalEnvironment& physical() const noexcept { return physical_; }
    void validate_invariants() const;
private:
    void on_result(SimTime at,ResultMessage result) override;
    void on_heartbeat(SimTime at,HeartbeatMessage message) override;
    void on_event(const core::AuditEvent& event) override;
    void on_event(const PhysicalEvent& event) override;
    void emit(const SimulationEvent& event);
    void schedule_timer(const core::TimeoutRequest& request);
    void timeout(const core::TimeoutRequest& request);
    void status_changed(NodeId node,core::NodeStatus before,core::NodeStatus after);
    void request_availability();
    void apply_availability();
    void request_dispatch();
    void dispatch();
    std::shared_ptr<const Scenario> scenario_;
    Variant variant_;
    engine::EventLoop loop_;
    core::Dispatcher dispatcher_;
    std::map<NodeId,core::Detector> detectors_;
    PhysicalEnvironment physical_;
    SimulationAuditSink* sink_;
    SimulationState state_ = SimulationState::ready;
    std::optional<SimTime> last_availability_request_;
    std::optional<SimTime> last_dispatch_request_;
    std::set<NodeId> newly_failed_;
};
}  // namespace telemetry::simulation
