#include "telemetry/simulation/simulation.hpp"
#include "telemetry/io/scenario_loader.hpp"

#include <array>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {
using namespace telemetry;
using namespace telemetry::simulation;
void require(bool value,std::string_view message) { if (!value) { throw std::runtime_error(std::string{message}); } }
template<class E,class F> void rejects(F action) {
    try { action(); } catch (const E&) { return; }
    throw std::runtime_error("Expected exception");
}
constexpr std::array variants{Variant::fixed_rr,Variant::fixed_ll,Variant::adaptive_rr,Variant::adaptive_ll};
PhysicalInput input(int end,int period = 2,int nodes = 2,bool lost = false) {
    PhysicalInput value{SimDuration{period},{},{},{},{}};
    for (int id = nodes; id >= 1; --id) {
        value.nodes.push_back({NodeId{id},1'000'000});
        for (int seq = end/period; seq >= 1; --seq) {
            value.deliveries.push_back({NodeId{id},HeartbeatSeq{seq},SimDuration{0},lost});
        }
    }
    return value;
}
std::shared_ptr<const Scenario> scenario(int end,PhysicalInput value,std::vector<TaskDescription> tasks = {},int tau = 4) {
    const auto period = value.heartbeat_period;
    return std::make_shared<const Scenario>(std::make_shared<const PhysicalTrace>(SimTime{end},std::move(value)),
        core::TimeoutParameters{period,SimDuration{tau},2},core::AdaptiveTimeout{3,2,2},std::move(tasks),
        std::vector<MeasurementWindow>{{SimTime{0},SimTime{end}}});
}
TaskDescription task(int id,int at = 0,long double work = 4) { return {TaskId{id},SimTime{at},work}; }
std::shared_ptr<const Scenario> example(std::string_view name) {
    return io::load_scenario(std::string{TELEMETRY_SOURCE_DIR}+"/examples/"+std::string{name}+"/scenario.json").scenario;
}
struct Recorder : SimulationAuditSink {
    Simulation* simulation = nullptr;
    std::vector<std::pair<engine::EventKey,SimulationEvent>> entries;
    std::function<void(const engine::EventKey&,const SimulationEvent&)> hook;
    void on_event(const engine::EventKey& key,const SimulationEvent& event) override {
        if (simulation) { simulation->validate_invariants(); }
        if (!entries.empty()) { require(entries.back().first <= key,"Audit keys out of order"); }
        entries.emplace_back(key,event);
        if (hook) { hook(key,event); }
    }
    template<class E> std::vector<E> events() const {
        std::vector<E> result;
        for (const auto& [key,event] : entries) {
            static_cast<void>(key);
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T,core::AuditEvent> || std::is_same_v<T,PhysicalEvent>) {
                    std::visit([&](const auto& nested) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(nested)>,E>) { result.push_back(nested); }
                    },value);
                } else if constexpr (std::is_same_v<T,E>) { result.push_back(value); }
            },event);
        }
        return result;
    }
    void batches() const {
        std::map<SimTime,int> reconfigs,dispatches;
        for (const auto& [key,event] : entries) {
            if (std::holds_alternative<DispatchBatch>(event)) {
                require(key.phase == phases::dispatch && ++dispatches[key.at] == 1,"Repeated/wrong-phase dispatch");
            }
            if (const auto* logical = std::get_if<core::AuditEvent>(&event)) {
                if (std::holds_alternative<core::ReconfigurationEvent>(*logical)) {
                    require(key.phase == phases::reconfiguration && ++reconfigs[key.at] == 1,"Repeated/wrong-phase reconfiguration");
                }
                if (std::holds_alternative<core::ResultEvent>(*logical)) { require(key.phase == phases::completion,"Result phase"); }
                if (std::holds_alternative<core::TaskArrivedEvent>(*logical)) { require(key.phase == phases::arrival,"Arrival phase"); }
                if (std::holds_alternative<core::TaskAssignedEvent>(*logical)) { require(key.phase == phases::dispatch,"Assignment phase"); }
            }
            if (std::holds_alternative<HeartbeatObserved>(event)) { require(key.phase == phases::heartbeat_receive,"Heartbeat phase"); }
            if (std::holds_alternative<TimeoutObserved>(event)) { require(key.phase == phases::timeout,"Timeout phase"); }
        }
    }
};
const core::TaskRecord& record(const Simulation& run,int id) { return run.dispatcher().task_store().tasks().at(TaskId{id}); }
void baseline() {
    const auto shared = example("baseline");
    for (const auto variant : variants) {
        Recorder audit; Simulation run{shared,variant,&audit}; audit.simulation = &run;
        require(run.run().reached == SimTime{20},"Baseline horizon");
        run.validate_invariants(); audit.batches();
        const bool rr = variant == Variant::fixed_rr || variant == Variant::adaptive_rr;
        const std::array choices = rr ? std::array{1,2,1,2} : std::array{1,2,2,1};
        const std::array times = rr ? std::array{4,2,8,4} : std::array{4,2,6,6};
        for (int id = 1; id <= 4; ++id) {
            const auto index = static_cast<std::size_t>(id-1);
            const auto& value = record(run,id);
            require(value.completed_at == SimTime{times[index]} && value.attempts.size() == 1 &&
                value.attempts.front().node == NodeId{choices[index]},"Manual baseline mismatch");
        }
        require(run.dispatcher().task_store().counts() == core::TaskCounts{4,4,0,0},"Baseline balance");
        require(audit.events<core::ResultEvent>().size() == 4,"Accepted once");
    }
}
void crash() {
    for (const auto variant : {Variant::fixed_rr,Variant::adaptive_rr}) {
        Recorder audit; Simulation run{example("crash"),variant,&audit}; audit.simulation = &run;
        static_cast<void>(run.run()); audit.batches();
        const std::array times{24,2,28,4};
        for (int id = 1; id <= 4; ++id) {
            require(record(run,id).completed_at == SimTime{times[static_cast<std::size_t>(id-1)]},"Crash completion mismatch");
        }
        const auto& first = record(run,1);
        require(first.attempts.size() == 2 && first.attempts[0].revoked_at == SimTime{20} &&
            first.attempts[1].assigned_at == SimTime{20},"Crash retry timing");
        require(run.physical().attempts().at({TaskId{1},AttemptNo{1}}).interrupted_at == SimTime{3},"Physical interruption");
        require(run.physical().attempts().at({TaskId{3},AttemptNo{1}}).started_at == std::nullopt,"Old crashed FIFO remains unstarted");
    }
    auto value = input(20,2,2); value.crashes.push_back({NodeId{1},SimTime{3}});
    Simulation run{scenario(20,std::move(value),{task(1,0,100),task(2,0,100),task(3,3,1)}),Variant::fixed_rr};
    static_cast<void>(run.run());
    require(record(run,3).attempts.front().node == NodeId{1} && record(run,3).attempts.front().assigned_at == SimTime{3},
        "Actual crash must not become immediate logical exclusion");
    require(!run.physical().attempts().at({TaskId{3},AttemptNo{1}}).started_at,"Assignment after crash remains unstarted");
}
void blackout() {
    for (const auto variant : variants) {
        Recorder audit; Simulation run{example("blackout"),variant,&audit};
        static_cast<void>(run.run()); audit.batches();
        bool suspect = false,restored = false;
        for (const auto& event : audit.events<TimeoutObserved>()) {
            if (event.request.node == NodeId{1} && event.update.after == core::NodeStatus::suspect) {
                suspect = suspect || event.at == SimTime{15};
            }
            require(event.update.after != core::NodeStatus::failed,"Short blackout caused FAILED");
        }
        for (const auto& event : audit.events<HeartbeatObserved>()) {
            if (event.message.node == NodeId{1} && event.at == SimTime{25}) {
                restored = event.update.before == core::NodeStatus::suspect && event.update.after == core::NodeStatus::alive;
            }
        }
        require(suspect && restored,"Short blackout recovery");
    }
    // False FAILED must not stop the old physical queue. Its late results are rejected.
    auto value = input(24,2,2);
    for (auto& delivery : value.deliveries) { if (delivery.node == NodeId{1}) { delivery.lost = true; } }
    Recorder audit; Simulation run{scenario(24,std::move(value),{task(1,0,10),task(2,0,1),task(3,0,3)}),Variant::fixed_rr,&audit};
    audit.simulation = &run; static_cast<void>(run.run()); audit.batches();
    const auto& old = run.physical().attempts().at({TaskId{3},AttemptNo{1}});
    require(old.started_at == SimTime{10} && old.finished_at == SimTime{13},"Logical FAILED cancelled old physical FIFO");
    require(record(run,1).completed_at == SimTime{18} && record(run,3).completed_at == SimTime{21},"Full-work retry and FIFO");
    std::size_t rejected = 0;
    for (const auto& event : audit.events<core::ResultEvent>()) { if (event.decision != core::ResultDecision::accepted) { ++rejected; } }
    require(rejected == 2,"Old physical results must be rejected");
}
void simultaneous() {
    auto value = input(16,2,3);
    for (auto& delivery : value.deliveries) { if (delivery.node != NodeId{3}) { delivery.lost = true; } }
    Recorder audit; Simulation run{scenario(16,std::move(value),{task(5,8,1),task(3,0,100),task(1,0,100),task(4,1,100),task(2,0,100)}),Variant::fixed_rr,&audit};
    static_cast<void>(run.run()); audit.batches();
    bool found = false;
    for (const auto& event : audit.events<core::ReconfigurationEvent>()) {
        if (event.at == SimTime{8}) {
            require(!found && event.newly_failed == std::vector<NodeId>{NodeId{1},NodeId{2}},"Joint FAILED missing");
            require(event.revoked.size() == 3 && event.revoked[0].task == TaskId{1} && event.revoked[1].task == TaskId{2} &&
                event.revoked[2].task == TaskId{4},"Global retry order");
            found = true;
        }
    }
    require(found,"No joint reconfiguration");
    std::vector<TaskId> assigned;
    for (const auto& event : audit.events<core::TaskAssignedEvent>()) { if (event.at == SimTime{8}) { assigned.push_back(event.assignment.task.id); } }
    require(assigned == std::vector<TaskId>{TaskId{1},TaskId{2},TaskId{4},TaskId{5}},"Dispatch must follow entire FAILED batch and arrivals");
    // No ALIVE nodes: queue persists; phase-4 recovery plus phase-7 arrival share phase 8.
    auto restored = input(12,2,2);
    for (auto& delivery : restored.deliveries) { delivery.lost = delivery.sequence.value() < 3; }
    Recorder log; Simulation recovered{scenario(12,std::move(restored),{task(2,5,1),task(1,4,1),task(3,6,1)}),Variant::fixed_rr,&log};
    static_cast<void>(recovered.run()); log.batches();
    require(record(recovered,1).attempts.front().assigned_at == SimTime{6} && record(recovered,3).attempts.front().assigned_at == SimTime{6},"Queue recovery");
    require(record(recovered,1).attempts.front().node == NodeId{1} && record(recovered,2).attempts.front().node == NodeId{2},"RR canonical restored order");
}
void boundaries() {
    // Completion at the FAILED deadline is accepted before phase 5; the second
    // old physical attempt starts before logical revocation and runs to its end.
    auto value = input(12,2,1,true); Recorder audit;
    Simulation run{scenario(12,std::move(value),{task(1,0,8),task(2,0,2),task(3,12,1)}),Variant::fixed_rr,&audit};
    static_cast<void>(run.run()); audit.batches();
    require(record(run,1).completed_at == SimTime{8} && record(run,1).attempts.size() == 1,"Completion before FAILED");
    require(run.physical().attempts().at({TaskId{2},AttemptNo{1}}).started_at == SimTime{8},"Immediate old FIFO start");
    require(!record(run,2).completed_at && run.dispatcher().task_store().counts() == core::TaskCounts{3,1,2,0},"Horizon balance");
    auto crashed = input(8,2,1); crashed.crashes.push_back({NodeId{1},SimTime{4}});
    Simulation collision{scenario(8,std::move(crashed),{task(1,0,4)}),Variant::fixed_rr}; static_cast<void>(collision.run());
    require(!record(collision,1).completed_at,"Crash must suppress same-time completion");
    auto edge = input(8,2,1,true);
    for (auto& delivery : edge.deliveries) { if (delivery.sequence == HeartbeatSeq{4}) { delivery.lost = false; } }
    Recorder endlog; Simulation end{scenario(8,std::move(edge),{task(1,8,1)}),Variant::fixed_rr,&endlog};
    static_cast<void>(end.run()); endlog.batches();
    require(end.detectors().at(NodeId{1}).status() == core::NodeStatus::alive && record(end,1).current_attempt == AttemptNo{1},"Heartbeat at FAILED horizon wins, arrival dispatch inclusive");
    auto inflight = input(8,2,1); inflight.crashes.push_back({NodeId{1},SimTime{3}});
    for (auto& delivery : inflight.deliveries) { delivery.delay = SimDuration{2}; }
    Simulation empty{scenario(8,std::move(inflight)),Variant::fixed_rr}; static_cast<void>(empty.run());
    require(empty.detectors().at(NodeId{1}).last_contact() == SimTime{4} && empty.detectors().at(NodeId{1}).status() == core::NodeStatus::suspect,
        "In-flight heartbeat at suspect deadline survives crash; empty task stream still runs");
}
// Canonical test signature includes all engine keys, event kinds, observations,
// decisions and final logical histories. It is not a production journal format.
std::string signature(const Recorder& audit,const Simulation& run,bool logical_only = false) {
    std::ostringstream out;
    for (const auto& [key,event] : audit.entries) {
        if (logical_only && std::holds_alternative<PhysicalEvent>(event)) { continue; }
        out << key.at.ticks() << ':' << key.phase.value() << ':';
        if (!logical_only) { out << key.insertion_sequence; for (auto x : key.tie_key) { out << ',' << x; } }
        out << ':' << event.index();
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T,core::AuditEvent> || std::is_same_v<T,PhysicalEvent>) {
                out << '/' << value.index();
                std::visit([&](const auto& nested) {
                    if constexpr (requires { nested.task.id; }) { out << 't' << nested.task.id.value(); }
                    if constexpr (requires { nested.assignment.task.id; }) { out << 'a' << nested.assignment.task.id.value() << ',' << nested.assignment.node.value() << ',' << nested.assignment.attempt.value(); }
                    if constexpr (requires { nested.result.task; }) { out << 'r' << nested.result.task.value() << ',' << nested.result.attempt.value() << ',' << nested.result.node.value(); }
                    if constexpr (requires { nested.decision; }) { out << 'd' << static_cast<int>(nested.decision); }
                    if constexpr (requires { nested.message.sequence; }) { out << 'h' << nested.message.node.value() << ',' << nested.message.sequence.value(); }
                    if constexpr (requires { nested.alive; }) {
                        for (auto id : nested.alive) { out << 'l' << id.value(); }
                        for (auto id : nested.newly_failed) { out << 'f' << id.value(); }
                        for (auto msg : nested.revoked) { out << 'v' << msg.task.value() << ',' << msg.attempt.value(); }
                    }
                },value);
            } else if constexpr (std::is_same_v<T,DispatchBatch>) { out << '/' << value.commands; }
            else {
                out << '/' << static_cast<int>(value.update.decision) << ',' << static_cast<int>(value.update.before) << ',' << static_cast<int>(value.update.after);
                if constexpr (std::is_same_v<T,HeartbeatObserved>) { out << ',' << value.message.node.value() << ',' << value.message.sequence.value() << ',' << value.generation; }
                else { out << ',' << value.request.node.value() << ',' << value.request.generation << ',' << static_cast<int>(value.request.kind); }
                out << ',' << value.thresholds.durations.suspect.ticks() << ',' << value.thresholds.durations.failed.ticks();
            }
        },event);
        out << '\n';
    }
    for (const auto& [id,value] : run.dispatcher().task_store().tasks()) {
        out << id.value() << '=' << (value.completed_at ? value.completed_at->ticks() : -1);
        for (const auto& attempt : value.attempts) {
            out << '/' << attempt.number.value() << ',' << attempt.node.value() << ',' << attempt.assigned_at.ticks() << ','
                << (attempt.revoked_at ? attempt.revoked_at->ticks() : -1) << ',' << (attempt.accepted_at ? attempt.accepted_at->ticks() : -1);
        }
        out << '\n';
    }
    return out.str();
}
void isolation() {
    const auto shared = example("crash"); std::map<Variant,std::string> results;
    for (auto variant : variants) {
        Recorder audit; Simulation run{shared,variant,&audit}; static_cast<void>(run.run()); results[variant] = signature(audit,run);
    }
    for (auto it = variants.rbegin(); it != variants.rend(); ++it) {
        Recorder audit; Simulation run{shared,*it,&audit}; static_cast<void>(run.run());
        require(results.at(*it) == signature(audit,run),"Repeat or variant-order state leak");
    }
    auto crashed = input(12,2,1,true); auto healthy = crashed;
    crashed.crashes.push_back({NodeId{1},SimTime{3}});
    Recorder a,b;
    Simulation first{scenario(12,std::move(crashed),{task(1,0,100),task(2,3,100)}),Variant::fixed_rr,&a};
    Simulation second{scenario(12,std::move(healthy),{task(1,0,100),task(2,3,100)}),Variant::fixed_rr,&b};
    static_cast<void>(first.run()); static_cast<void>(second.run());
    require(first.physical().nodes().at(NodeId{1}).crashed && !second.physical().nodes().at(NodeId{1}).crashed,"Different hidden states");
    require(signature(a,first,true) == signature(b,second,true),"Hidden crash leaked into logical decisions");
    require(first.dispatcher().task_store().tasks() == second.dispatcher().task_store().tasks(),"Hidden state changed histories");
}
void errors() {
    const auto shared = example("baseline");
    rejects<std::invalid_argument>([&] { Simulation run{nullptr,Variant::fixed_rr}; });
    rejects<std::invalid_argument>([&] { Simulation run{shared,static_cast<Variant>(123)}; });
    Simulation limited{shared,Variant::fixed_rr,nullptr,0};
    rejects<engine::EventLimitExceeded>([&] { static_cast<void>(limited.run()); });
    require(limited.state() == SimulationState::failed && limited.clock().state() == engine::RunState::failed,"Limit must fail explicitly");
    rejects<std::logic_error>([&] { static_cast<void>(limited.run()); });
    Recorder audit; Simulation throwing{shared,Variant::fixed_rr,&audit};
    audit.hook = [](const auto&,const auto&) { throw std::runtime_error("Sink error"); };
    rejects<std::runtime_error>([&] { static_cast<void>(throwing.run()); });
    require(throwing.state() == SimulationState::failed && throwing.clock().state() == engine::RunState::failed,"Sink failure must stop execution");
    rejects<std::logic_error>([&] { static_cast<void>(throwing.run()); });
    Recorder reentrant; Simulation run{shared,Variant::fixed_rr,&reentrant};
    reentrant.hook = [&](const auto&,const auto&) { rejects<std::logic_error>([&] { static_cast<void>(run.run()); }); };
    static_cast<void>(run.run()); rejects<std::logic_error>([&] { static_cast<void>(run.run()); });
}
}
int main(int argc,char** argv) {
    try {
        if (argc != 2) { throw std::invalid_argument("Expected group"); }
        const std::map<std::string_view,void(*)()> groups{{"baseline",baseline},{"crash",crash},{"blackout",blackout},
            {"simultaneous",simultaneous},{"boundaries",boundaries},{"isolation",isolation},{"errors",errors}};
        groups.at(argv[1])();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
