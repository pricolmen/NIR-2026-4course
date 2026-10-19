#include "telemetry/simulation/simulation.hpp"

#include <stdexcept>
#include <utility>

namespace telemetry::simulation {
namespace {
std::shared_ptr<const Scenario> checked_scenario(std::shared_ptr<const Scenario> scenario) {
    if (!scenario) { throw std::invalid_argument("Simulation requires a scenario"); }
    return scenario;
}
Variant checked_variant(Variant variant) {
    switch (variant) {
    case Variant::fixed_rr: case Variant::fixed_ll: case Variant::adaptive_rr: case Variant::adaptive_ll: return variant;
    }
    throw std::invalid_argument("Unknown simulation variant");
}
core::SchedulingPolicy scheduling(Variant variant) {
    return variant == Variant::fixed_rr || variant == Variant::adaptive_rr
        ? core::SchedulingPolicy::round_robin : core::SchedulingPolicy::least_loaded;
}
core::TimeoutPolicy timeout_policy(Variant variant,const Scenario& scenario) {
    if (variant == Variant::fixed_rr || variant == Variant::fixed_ll) { return core::FixedTimeout{}; }
    return scenario.adaptive();
}
}
Simulation::Simulation(std::shared_ptr<const Scenario> scenario,Variant variant,SimulationAuditSink* sink,std::uint64_t max_events)
    : scenario_(checked_scenario(std::move(scenario))),variant_(checked_variant(variant)),
      loop_(scenario_->physical()->horizon(),max_events),
      dispatcher_(std::vector<NodeDefinition>{scenario_->physical()->nodes().begin(),scenario_->physical()->nodes().end()},
                  scheduling(variant_),this),
      physical_(loop_,scenario_->physical(),*this,this),sink_(sink) {
    for (const auto& node : scenario_->physical()->nodes()) {
        detectors_.try_emplace(node.id,node.id,scenario_->timeout(),timeout_policy(variant_,*scenario_));
    }
}
engine::RunResult Simulation::run() {
    if (state_ != SimulationState::ready) { throw std::logic_error("Simulation can run only once"); }
    state_ = SimulationState::running;
    try {
        physical_.initialize();
        for (const auto& [node,detector] : detectors_) {
            static_cast<void>(node);
            for (const auto& timer : detector.timer_requests(loop_.horizon())) { schedule_timer(timer); }
        }
        for (const auto& task : scenario_->tasks()) {
            static_cast<void>(loop_.schedule(task.arrival,phases::arrival,{task.id.value(),0,0,0},
                [this,task](engine::EventLoop&) { dispatcher_.arrive(loop_.now(),task); request_dispatch(); }));
        }
        const auto result = loop_.run();
        state_ = SimulationState::completed;
        validate_invariants();
        return result;
    } catch (...) { state_ = SimulationState::failed; throw; }
}
void Simulation::emit(const SimulationEvent& event) {
    if (sink_) { sink_->on_event(*loop_.last_event(),event); }
}
void Simulation::on_event(const core::AuditEvent& event) { emit(event); }
void Simulation::on_event(const PhysicalEvent& event) { emit(event); }
void Simulation::on_result(SimTime at,ResultMessage result) {
    static_cast<void>(dispatcher_.receive_result(at,result));
    request_dispatch();
}
void Simulation::on_heartbeat(SimTime at,HeartbeatMessage message) {
    auto& detector = detectors_.at(message.node);
    auto update = detector.receive_heartbeat(at,message,loop_.horizon());
    for (const auto& timer : update.timers) { schedule_timer(timer); }
    status_changed(message.node,update.before,update.after);
    emit(HeartbeatObserved{at,message,std::move(update),detector.generation(),detector.last_contact(),detector.thresholds()});
}
void Simulation::schedule_timer(const core::TimeoutRequest& request) {
    const std::uint64_t kind = request.kind == core::TimeoutKind::suspect ? 0 : 1;
    static_cast<void>(loop_.schedule(request.at,phases::timeout,{request.node.value(),kind,request.generation,0},
        [this,request](engine::EventLoop&) { timeout(request); }));
}
void Simulation::timeout(const core::TimeoutRequest& request) {
    auto& detector = detectors_.at(request.node);
    const auto update = detector.receive_timeout(loop_.now(),request);
    status_changed(request.node,update.before,update.after);
    emit(TimeoutObserved{loop_.now(),request,update,detector.thresholds()});
}
void Simulation::status_changed(NodeId node,core::NodeStatus before,core::NodeStatus after) {
    if (before == after) { return; }
    if (after == core::NodeStatus::failed) { newly_failed_.insert(node); }
    request_availability();
}
void Simulation::request_availability() {
    if (last_availability_request_ == loop_.now()) { return; }
    static_cast<void>(loop_.schedule(loop_.now(),phases::reconfiguration,{},
        [this](engine::EventLoop&) { apply_availability(); }));
    last_availability_request_ = loop_.now();
}
void Simulation::apply_availability() {
    std::vector<NodeId> alive;
    for (const auto& [node,detector] : detectors_) {
        if (detector.status() == core::NodeStatus::alive) { alive.push_back(node); }
    }
    // Availability depends only on detector observations, never physical state.
    static_cast<void>(dispatcher_.apply_availability(loop_.now(),std::move(alive),
        std::vector<NodeId>{newly_failed_.begin(),newly_failed_.end()}));
    newly_failed_.clear();
    request_dispatch();
}
void Simulation::request_dispatch() {
    if (last_dispatch_request_ == loop_.now()) { return; }
    static_cast<void>(loop_.schedule(loop_.now(),phases::dispatch,{},
        [this](engine::EventLoop&) { dispatch(); }));
    last_dispatch_request_ = loop_.now();
}
void Simulation::dispatch() {
    const auto commands = dispatcher_.dispatch(loop_.now());
    for (const auto& command : commands) { physical_.submit(command); }
    emit(DispatchBatch{loop_.now(),commands.size()});
}
void Simulation::validate_invariants() const {
    dispatcher_.validate_invariants(); physical_.validate_invariants();
    if (state_ != SimulationState::completed) { return; }
    const auto require = [](bool value) { if (!value) { throw std::logic_error("Integrated simulation invariant violated"); } };
    require(loop_.state() == engine::RunState::completed && loop_.now() == loop_.horizon() && newly_failed_.empty());
    require(detectors_.size() == scenario_->physical()->nodes().size());
    std::vector<NodeId> alive,failed;
    for (const auto& [id,detector] : detectors_) {
        require(detector.node() == id && detector.last_contact() <= loop_.now());
        if (detector.status() == core::NodeStatus::alive) { alive.push_back(id); }
        if (detector.status() == core::NodeStatus::failed) { failed.push_back(id); }
    }
    require(alive == std::vector<NodeId>{dispatcher_.alive().begin(),dispatcher_.alive().end()});
    require(failed == std::vector<NodeId>{dispatcher_.failed().begin(),dispatcher_.failed().end()});
    require(dispatcher_.task_store().tasks().size() == scenario_->tasks().size());
    std::size_t issued = 0;
    for (const auto& [id,task] : dispatcher_.task_store().tasks()) {
        for (const auto& attempt : task.attempts) {
            const auto& physical = physical_.attempts().at({id,attempt.number});
            require(physical.assignment.task == task.description && physical.assignment.node == attempt.node &&
                    physical.assigned_at == attempt.assigned_at);
            if (attempt.accepted_at) { require(physical.finished_at == attempt.accepted_at); }
            if (task.current_attempt == attempt.number) { require(!physical.finished_at); }
            ++issued;
        }
    }
    require(issued == physical_.attempts().size());
}
}  // namespace telemetry::simulation
