#include "telemetry/simulation/physical_environment.hpp"

#include <set>
#include <stdexcept>
#include <utility>

namespace telemetry::simulation {
namespace {
struct Operation {
    bool& active;
    explicit Operation(bool& value) : active(value) {
        if (active) { throw std::logic_error("Reentrant physical environment mutation"); }
        active = true;
    }
    ~Operation() { active = false; }
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
};
ResultMessage result(const AssignmentCommand& command) { return {command.task.id,command.attempt,command.node}; }
engine::TieKey attempt_tie(NodeId node,PhysicalAttemptKey key) { return {node.value(),key.task.value(),key.attempt.value(),0}; }
}
PhysicalEnvironment::PhysicalEnvironment(engine::EventLoop& loop,std::shared_ptr<const PhysicalTrace> trace,
                                         ObservationSink& observations,PhysicalAuditSink* audit)
    : loop_(loop),trace_(std::move(trace)),observations_(observations),audit_(audit) {
    if (!trace_ || trace_->horizon() != loop_.horizon() || loop_.state() != engine::RunState::ready) {
        throw std::invalid_argument("Physical trace must match a ready event loop");
    }
    for (const auto& node : trace_->nodes()) { nodes_.emplace(node.id,PhysicalNode{node,false,std::nullopt,{},0,0}); }
}
void PhysicalEnvironment::require_phase(engine::Phase phase) const {
    if (!initialized_ || loop_.state() != engine::RunState::running || !loop_.last_event() ||
        loop_.last_event()->phase != phase) { throw std::logic_error("Physical operation in invalid lifecycle/phase"); }
}
void PhysicalEnvironment::emit(const PhysicalEvent& event) { if (audit_) { audit_->on_event(event); } }
void PhysicalEnvironment::initialize() {
    Operation operation{operating_};
    if (initialization_attempted_ || loop_.state() != engine::RunState::ready) {
        throw std::logic_error("Initialize environment once before run");
    }
    initialization_attempted_ = true;
    for (const auto& item : trace_->crashes()) {
        static_cast<void>(loop_.schedule(item.at,phases::crash,{item.node.value(),0,0,0},
            [this,node=item.node](engine::EventLoop&) { crash(node); }));
    }
    if (const auto at = deadline_within(SimTime{},trace_->heartbeat_period(),loop_.horizon())) {
        for (const auto& [id,node] : nodes_) {
            static_cast<void>(node);
            static_cast<void>(loop_.schedule(*at,phases::heartbeat_send,{id.value(),1,0,0},
                [this,id](engine::EventLoop&) { send(id,HeartbeatSeq{1}); }));
        }
    }
    initialized_ = true;
}
std::optional<PhysicalStarted> PhysicalEnvironment::start_next(PhysicalNode& node) {
    if (node.crashed || node.running || node.waiting.empty()) { return std::nullopt; }
    const auto key = node.waiting.front();
    auto& attempt = attempts_.at(key);
    const auto token = checked_increment(node.completion_token);
    const auto due = deadline_within(loop_.now(),attempt.duration,loop_.horizon());
    if (due) {
        static_cast<void>(loop_.schedule(*due,phases::completion,attempt_tie(node.definition.id,key),
            [this,id=node.definition.id,key,token](engine::EventLoop&) { complete(id,key,token); }));
    }
    node.completion_token = token;
    node.running = key;
    node.waiting.pop_front();
    attempt.started_at = loop_.now();
    attempt.completion_due = due;
    attempt.completion_token = token;
    return PhysicalStarted{loop_.now(),result(attempt.assignment),attempt.duration,due,token};
}
void PhysicalEnvironment::submit(AssignmentCommand command) {
    Operation operation{operating_}; require_phase(phases::dispatch);
    auto& node = nodes_.at(command.node);
    if (command.task.arrival > loop_.now()) { throw std::invalid_argument("Assignment precedes task arrival"); }
    const auto duration = processing_duration(command.task.work,node.definition.performance);
    const PhysicalAttemptKey key{command.task.id,command.attempt};
    if (attempts_.contains(key)) { throw std::invalid_argument("Duplicate physical attempt"); }
    node.waiting.push_back(key);
    try {
        attempts_.emplace(key,PhysicalAttempt{command,loop_.now(),duration,std::nullopt,std::nullopt,
                                            std::nullopt,std::nullopt,std::nullopt});
    } catch (...) { node.waiting.pop_back(); throw; }
    const auto started = start_next(node);
    emit(PhysicalAssigned{loop_.now(),command});
    if (started) { emit(*started); }
}
void PhysicalEnvironment::crash(NodeId id) {
    Operation operation{operating_}; require_phase(phases::crash);
    auto& node = nodes_.at(id);
    const auto token = checked_increment(node.completion_token);
    std::optional<ResultMessage> interrupted;
    if (node.running) {
        auto& attempt = attempts_.at(*node.running);
        interrupted = result(attempt.assignment);
        attempt.interrupted_at = loop_.now();
    }
    node.crashed = true;
    node.completion_token = token;
    node.running.reset();
    // Waiting commands, including later assignments to this crashed node,
    // remain inspectable and can never start. No restart exists in the model.
    emit(PhysicalCrash{loop_.now(),id,interrupted});
}
void PhysicalEnvironment::complete(NodeId id,PhysicalAttemptKey key,std::uint64_t token) {
    Operation operation{operating_}; require_phase(phases::completion);
    auto& node = nodes_.at(id);
    auto& attempt = attempts_.at(key);
    if (node.crashed || node.running != key || node.completion_token != token) {
        emit(PhysicalCompletionSkipped{loop_.now(),result(attempt.assignment),token,node.completion_token,
            node.crashed ? CompletionSkipReason::crashed : CompletionSkipReason::stale_token});
        return;
    }
    attempt.finished_at = loop_.now();
    node.running.reset();
    emit(PhysicalFinished{loop_.now(),result(attempt.assignment)});
    observations_.on_result(loop_.now(),result(attempt.assignment));
    // The result reaches core synchronously BEFORE starting the next old FIFO
    // attempt; new dispatch happens separately in phase 8.
    const auto started = start_next(node);
    if (started) { emit(*started); }
}
void PhysicalEnvironment::send(NodeId id,HeartbeatSeq sequence) {
    Operation operation{operating_}; require_phase(phases::heartbeat_send);
    auto& node = nodes_.at(id);
    const HeartbeatMessage message{id,sequence};
    if (node.crashed) { emit(PhysicalSendSkipped{loop_.now(),message}); return; }
    if (sequence.value() != checked_increment(node.sent_heartbeats)) { throw std::logic_error("Nonconsecutive physical heartbeat send"); }
    const auto& delivery = trace_->delivery(id,sequence);
    const auto receive_at = deadline_within(loop_.now(),delivery.delay,loop_.horizon());
    const auto disposition = delivery.lost ? DeliveryDisposition::trace_lost : !receive_at ? DeliveryDisposition::beyond_horizon :
        trace_->blocked(id,*receive_at) ? DeliveryDisposition::blackout : DeliveryDisposition::scheduled;
    if (disposition == DeliveryDisposition::scheduled) {
        static_cast<void>(loop_.schedule(*receive_at,phases::heartbeat_receive,{id.value(),sequence.value(),0,0},
            [this,sent_at=loop_.now(),message](engine::EventLoop&) { deliver(sent_at,message); }));
    }
    if (const auto next = deadline_within(loop_.now(),trace_->heartbeat_period(),loop_.horizon())) {
        const auto next_sequence = sequence.next();
        static_cast<void>(loop_.schedule(*next,phases::heartbeat_send,{id.value(),next_sequence.value(),0,0},
            [this,id,next_sequence](engine::EventLoop&) { send(id,next_sequence); }));
    }
    node.sent_heartbeats = sequence.value();
    emit(PhysicalHeartbeatSent{loop_.now(),message,delivery.delay,disposition,receive_at});
}
void PhysicalEnvironment::deliver(SimTime sent_at,HeartbeatMessage message) {
    Operation operation{operating_}; require_phase(phases::heartbeat_receive);
    // A previously sent message survives physical crash and preserves its
    // planned receipt time; sequence admissibility belongs to core.
    emit(PhysicalHeartbeatDelivered{loop_.now(),sent_at,message});
    observations_.on_heartbeat(loop_.now(),message);
}
void PhysicalEnvironment::validate_invariants() const {
    const auto require = [](bool value) { if (!value) { throw std::logic_error("Physical environment invariant violated"); } };
    std::set<PhysicalAttemptKey> resident;
    for (const auto& [id,node] : nodes_) {
        require(id == node.definition.id);
        if (node.crashed) { require(!node.running); }
        if (node.running) {
            require(resident.insert(*node.running).second);
            const auto& attempt = attempts_.at(*node.running);
            require(attempt.assignment.node == id && attempt.started_at && !attempt.finished_at && !attempt.interrupted_at);
            require(attempt.completion_token == node.completion_token);
        }
        for (const auto key : node.waiting) {
            require(resident.insert(key).second);
            const auto& attempt = attempts_.at(key);
            require(attempt.assignment.node == id && !attempt.started_at && !attempt.finished_at && !attempt.interrupted_at);
        }
    }
    for (const auto& [key,attempt] : attempts_) {
        require(key.task == attempt.assignment.task.id && key.attempt == attempt.assignment.attempt);
        require(nodes_.contains(attempt.assignment.node));
        require(attempt.assigned_at >= attempt.assignment.task.arrival && attempt.duration.ticks() > 0);
        require(!(attempt.finished_at && attempt.interrupted_at));
        require(attempt.started_at.has_value() == attempt.completion_token.has_value());
        if (attempt.started_at) { require(*attempt.started_at >= attempt.assigned_at); }
        if (attempt.completion_due) {
            require(attempt.started_at && *attempt.completion_due > *attempt.started_at && *attempt.completion_due <= loop_.horizon());
            require(*attempt.completion_due - *attempt.started_at == attempt.duration);
        }
        if (attempt.finished_at) {
            require(attempt.started_at && attempt.finished_at == attempt.completion_due && !resident.contains(key));
        } else if (attempt.interrupted_at) {
            require(attempt.started_at && *attempt.interrupted_at >= *attempt.started_at && !resident.contains(key));
            require(nodes_.at(attempt.assignment.node).crashed);
        } else { require(resident.contains(key)); }
    }
}
}  // namespace telemetry::simulation
