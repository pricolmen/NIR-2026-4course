#include "telemetry/core/dispatcher.hpp"
#include "telemetry/core/detector.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace telemetry;
using namespace telemetry::core;
void require(bool ok, std::string_view message) {
    if (!ok) { throw std::runtime_error(std::string{message}); }
}
template<class Exception, class Action> void rejects(Action action, std::string_view message) {
    try { action(); } catch (const Exception&) { return; }
    throw std::runtime_error(std::string{message});
}
std::vector<NodeDefinition> nodes() { return {{NodeId{3},2},{NodeId{1},1},{NodeId{2},1}}; }
void add(Dispatcher& d, int id, int at = 0, long double work = 4) {
    d.arrive(SimTime{at},{TaskId{id},SimTime{at},work});
}
ResultMessage result(const AssignmentCommand& a) { return {a.task.id,a.attempt,a.node}; }
std::vector<TaskId> pending(const Dispatcher& d) {
    return {d.task_store().pending().begin(),d.task_store().pending().end()};
}
std::vector<TaskId> ids(const std::vector<ResultMessage>& results) {
    std::vector<TaskId> value;
    for (const auto& r : results) { value.push_back(r.task); }
    return value;
}
std::vector<TaskId> ids(const std::vector<AssignmentCommand>& commands) {
    std::vector<TaskId> value;
    for (const auto& c : commands) { value.push_back(c.task.id); }
    return value;
}
void zero(const Dispatcher& d, NodeId node) {
    const auto& load = d.loads().at(node);
    require(load.sum == 0 && load.compensation == 0 && load.assigned_work() == 0 && load.assignments == 0,
            "Excluded node must have exact zero work/count/compensation");
}

void batch() {
    Dispatcher d{nodes(),SchedulingPolicy::round_robin};
    for (const auto id : {90,10,70}) { add(d,id); }
    const auto first = d.dispatch(SimTime{0});
    for (const auto id : {5,4,3}) { add(d,id,1,2); }
    const auto second = d.dispatch(SimTime{1});
    require(first[0].node == NodeId{1} && first[1].node == NodeId{2} &&
            second[0].node == NodeId{1} && second[1].node == NodeId{2}, "Test assignments");
    d.set_alive(SimTime{2},{});
    add(d,800,2,8); add(d,600,2,6);
    require(d.dispatch(SimTime{2}).empty(), "Tasks must wait without ALIVE");
    const auto report = d.apply_availability(SimTime{3},{NodeId{3}},{NodeId{2},NodeId{1}});
    const std::vector<TaskId> returned{TaskId{10},TaskId{90},TaskId{4},TaskId{5}};
    const std::vector<TaskId> expected{TaskId{800},TaskId{600},TaskId{10},TaskId{90},TaskId{4},TaskId{5}};
    require(report.newly_failed == std::vector<NodeId>{NodeId{1},NodeId{2}} && ids(report.revoked) == returned,
            "Joint return must sort by original arrival, then ID across all nodes");
    require(pending(d) == expected && report.alive == std::vector<NodeId>{NodeId{3}},
            "Sorted union appended after unchanged existing FIFO");
    require(d.task_store().counts() == TaskCounts{8,0,6,2}, "Reconfiguration must not dispatch");
    zero(d,NodeId{1}); zero(d,NodeId{2});
    require(d.loads().at(NodeId{3}).assigned_work() == 6, "Unaffected node lost current/local-queued full work");
    for (const auto& old : report.revoked) {
        const auto& record = d.task_store().tasks().at(old.task);
        require(!record.current_attempt && record.attempts.back().revoked_at == SimTime{3}, "Missing logical revocation");
    }
    const auto again = d.apply_availability(SimTime{3},{NodeId{3}},{NodeId{1},NodeId{2}});
    require(again.newly_failed.empty() && again.revoked.empty() && pending(d) == expected,
            "Repeated exclusion returned tasks twice");
    add(d,700,3,7); // Phase 7, after joint return, before one phase-8 dispatch.
    auto with_arrival = expected; with_arrival.push_back(TaskId{700});
    const auto retry = d.dispatch(SimTime{3});
    require(ids(retry) == with_arrival, "Dispatch must preserve old FIFO, joint return and new arrival order");
    for (const auto& command : retry) {
        require(command.node == NodeId{3}, "Excluded node received an assignment");
        const auto was_returned = std::find(returned.begin(),returned.end(),command.task.id) != returned.end();
        require(command.attempt == AttemptNo{was_returned ? 2 : 1}, "Retry attempt number");
        require(command.task == d.task_store().tasks().at(command.task.id).description, "Retry lost original arrival/work");
    }
    const auto load = d.loads().at(NodeId{3}).assigned_work();
    const auto repeated = d.apply_availability(SimTime{4},{NodeId{3}},{NodeId{2},NodeId{1}});
    require(repeated.revoked.empty() && d.loads().at(NodeId{3}).assigned_work() == load,
            "Repeated old FAILED affected reassigned work");
    d.validate_invariants();
    const auto all = d.apply_availability(SimTime{5},{},{NodeId{3}});
    require(all.revoked.size() == 9 && d.dispatch(SimTime{5}).empty() && pending(d).size() == 9,
            "No ALIVE must retain every unfinished task once");
    require(ids(all.revoked) == std::vector<TaskId>{TaskId{10},TaskId{70},TaskId{90},TaskId{3},TaskId{4},
                                                  TaskId{5},TaskId{600},TaskId{800},TaskId{700}},
            "Later joint return used last assignment time instead of original arrival");
    zero(d,NodeId{3}); d.validate_invariants();
}

void results() {
    for (const auto policy : {SchedulingPolicy::round_robin,SchedulingPolicy::least_loaded}) {
        Dispatcher d{nodes(),policy};
        d.set_alive(SimTime{0},{NodeId{1}});
        add(d,1,0,8); add(d,2,0,2); add(d,3,0,3);
        const auto physical_queue = d.dispatch(SimTime{0});
        require(d.receive_result(SimTime{4},result(physical_queue[0])) == ResultDecision::accepted,
                "Phase-2 completion before FAILED must be accepted");
        const auto report = d.apply_availability(SimTime{4},{},{NodeId{1}});
        require(ids(report.revoked) == std::vector<TaskId>{TaskId{2},TaskId{3}}, "Completed task returned");
        // Physical environment keeps its previously issued commands. Logical
        // revocation produces no command that clears/stops this external queue.
        require(physical_queue.size() == 3 && physical_queue[1].task.work == 2, "Physical commands changed");
        for (std::size_t i = 1; i < physical_queue.size(); ++i) {
            require(d.receive_result(SimTime{5},result(physical_queue[i])) == ResultDecision::no_current_assignment,
                    "Old physical result accepted before reassignment");
        }
        zero(d,NodeId{1});
        d.set_alive(SimTime{6},{NodeId{2}});
        const auto retry = d.dispatch(SimTime{6});
        require(retry.size() == 2 && retry[0].attempt == AttemptNo{2} && retry[0].task.work == 2 &&
                retry[1].task.work == 3 && retry[0].task.arrival == SimTime{0}, "Full original work on retry");
        const auto before = d.task_store().counts();
        for (std::size_t i = 1; i < physical_queue.size(); ++i) {
            require(d.receive_result(SimTime{7},result(physical_queue[i])) == ResultDecision::attempt_mismatch,
                    "Old physical result accepted after reassignment");
        }
        require(d.loads().at(NodeId{2}).assigned_work() == 5 && d.task_store().counts() == before,
                "Old results changed new load/counts");
        require(d.receive_result(SimTime{7},{retry[0].task.id,retry[0].attempt,NodeId{1}}) == ResultDecision::node_mismatch,
                "Current attempt with wrong executor accepted");
        for (const auto& a : retry) {
            require(d.receive_result(SimTime{8},result(a)) == ResultDecision::accepted, "Current retry rejected");
            require(d.receive_result(SimTime{8},result(a)) == ResultDecision::already_completed, "Duplicate retry accepted");
        }
        for (const auto& old : physical_queue) {
            require(d.receive_result(SimTime{9},result(old)) == ResultDecision::already_completed, "Old result after completion accepted");
        }
        zero(d,NodeId{2}); require(d.task_store().counts() == TaskCounts{3,3,0,0}, "Final task balance");
        d.validate_invariants();
    }
    // A new attempt on the SAME node is still distinct from its old attempt.
    Dispatcher repeated{nodes(),SchedulingPolicy::round_robin};
    repeated.set_alive(SimTime{0},{NodeId{1}}); add(repeated,1);
    const auto first = repeated.dispatch(SimTime{0});
    require(repeated.revoke_assignment(SimTime{1},TaskId{1}), "Manual logical revoke");
    const auto second = repeated.dispatch(SimTime{1});
    require(repeated.receive_result(SimTime{2},result(first[0])) == ResultDecision::attempt_mismatch,
            "Same executor made old attempt valid");
    require(repeated.loads().at(NodeId{1}).assigned_work() == 4, "Same-node old attempt removed new load");
    const auto failed = repeated.apply_availability(SimTime{3},{},{NodeId{1}});
    require(failed.revoked.size() == 1 && failed.revoked[0].attempt == second[0].attempt &&
            repeated.task_store().tasks().at(TaskId{1}).attempts.size() == 2, "Batch did not revoke current retry exactly once");
    require(!repeated.revoke_assignment(SimTime{3},TaskId{1}), "Single revoke duplicated joint return");
    repeated.validate_invariants();
}

void availability() {
    Dispatcher d{nodes(),SchedulingPolicy::round_robin};
    add(d,1); static_cast<void>(d.dispatch(SimTime{0})); // next = 2
    auto report = d.apply_availability(SimTime{1},{NodeId{3}},{NodeId{2}}); // node 1 SUSPECT, node 2 FAILED
    require(report.revoked.empty() && d.loads().at(NodeId{1}).assigned_work() == 4,
            "SUSPECT must preserve existing work");
    report = d.apply_availability(SimTime{2},{NodeId{3},NodeId{1}},{}); // recover 1, do not reset next=3
    add(d,2,2); require(d.dispatch(SimTime{2})[0].node == NodeId{3}, "SUSPECT recovery reset RR");
    d.set_alive(SimTime{3},{}); // preserve next = 1 across empty interval
    static_cast<void>(d.apply_availability(SimTime{4},{NodeId{3},NodeId{1}},{NodeId{2}}));
    add(d,3,4); require(d.dispatch(SimTime{4})[0].node == NodeId{1}, "Empty ALIVE lost RR anchor");
    require(d.failed().size() == 1 && d.failed()[0] == NodeId{2}, "Terminal failure record");
    d.validate_invariants();
    // Join detector observations manually; no physical-crash input is passed.
    Dispatcher joined{nodes(),SchedulingPolicy::round_robin};
    Detector detector{NodeId{1},{SimDuration{10},SimDuration{20},2},FixedTimeout{}};
    const auto timers = detector.timer_requests(SimTime{100});
    joined.set_alive(SimTime{0},{NodeId{1}});
    add(joined,1); const auto commands = joined.dispatch(SimTime{0});
    // Suppose physical crash happened at 5. Without a detector observation,
    // a new assignment at 10 must still be made and later returned too.
    add(joined,2,10); const auto after_crash = joined.dispatch(SimTime{10});
    require(after_crash[0].node == NodeId{1}, "Core used hidden crash knowledge");
    auto transition = detector.receive_timeout(timers[0].at,timers[0]);
    require(transition.after == NodeStatus::suspect, "Detector suspect transition");
    report = joined.apply_availability(timers[0].at,{NodeId{2},NodeId{3}},{});
    require(report.revoked.empty(), "Detector SUSPECT triggered return");
    // Assignments to a physically crashed but not yet excluded node would be
    // treated identically: core has no access to the crash instant.
    transition = detector.receive_timeout(timers[1].at,timers[1]);
    require(transition.after == NodeStatus::failed, "Detector failed transition");
    report = joined.apply_availability(timers[1].at,{NodeId{2},NodeId{3}},{detector.node()});
    require(report.revoked.size() == 2 && report.revoked[0] == result(commands[0]) &&
            report.revoked[1] == result(after_crash[0]), "FAILED missed assignments made after unobserved crash");
    joined.validate_invariants();
}

struct Observer : AuditSink {
    Dispatcher* d = nullptr;
    std::vector<AuditEvent> events;
    bool fail = false;
    bool reenter = false;
    void on_event(const AuditEvent& event) override {
        if (d) { d->validate_invariants(); }
        if (const auto* batch = std::get_if<ReconfigurationEvent>(&event)) {
            for (const auto node : batch->newly_failed) { zero(*d,node); }
            require(std::vector<NodeId>{d->alive().begin(),d->alive().end()} == batch->alive,
                    "Callback observed intermediate availability");
            require(pending(*d).size() >= batch->revoked.size(), "Callback observed incomplete FIFO return");
            if (reenter) { static_cast<void>(d->apply_availability(batch->at,{},{})); }
            if (fail) { throw std::runtime_error("Journal failure"); }
        }
        events.push_back(event);
    }
};
void audit() {
    Observer observer;
    Dispatcher d{nodes(),SchedulingPolicy::round_robin,&observer}; observer.d = &d;
    for (int i = 1; i <= 6; ++i) { add(d,i); }
    static_cast<void>(d.dispatch(SimTime{0}));
    const auto before = observer.events.size();
    const auto report = d.apply_availability(SimTime{1},{NodeId{3}},{NodeId{2},NodeId{1}});
    require(observer.events.size() == before+1, "Batch should emit one coherent event");
    const auto& event = std::get<ReconfigurationEvent>(observer.events.back());
    require(event.revoked == report.revoked && event.newly_failed == report.newly_failed &&
            event.alive == report.alive && event.at == SimTime{1}, "Batch journal payload");
    observer.fail = true;
    rejects<std::runtime_error>([&] { static_cast<void>(d.apply_availability(SimTime{2},{},{NodeId{3}})); },
                                "Journal exception swallowed");
    require(d.failed().size() == 3 && d.alive().empty() && pending(d).size() == 6,
            "Journal exception left partial logical batch");
    d.validate_invariants(); observer.fail = false;
    const auto again = d.apply_availability(SimTime{2},{},{NodeId{3}});
    require(again.revoked.empty(), "Retry after journal failure duplicated revocations");
    Observer guarded;
    Dispatcher reentrant{nodes(),SchedulingPolicy::round_robin,&guarded}; guarded.d = &reentrant;
    add(reentrant,1); static_cast<void>(reentrant.dispatch(SimTime{0})); guarded.reenter = true;
    rejects<std::logic_error>([&] { static_cast<void>(reentrant.apply_availability(SimTime{1},{},{NodeId{1}})); },
                             "Audit callback reentered batch mutation");
    require(reentrant.failed().size() == 1 && pending(reentrant).size() == 1, "Reentrant callback changed committed batch");
    reentrant.validate_invariants();
}

void inputs() {
    Dispatcher d{nodes(),SchedulingPolicy::round_robin};
    add(d,1); const auto original = d.dispatch(SimTime{0});
    const auto record = d.task_store().tasks().at(TaskId{1});
    for (const auto& alive : {std::vector<NodeId>{NodeId{1},NodeId{1}},std::vector<NodeId>{NodeId{99}},
                            std::vector<NodeId>{NodeId{1},NodeId{3}}}) {
        rejects<std::invalid_argument>([&] { static_cast<void>(d.apply_availability(SimTime{10},alive,{NodeId{1}})); },
                                       "Invalid final ALIVE set");
    }
    for (const auto& failed : {std::vector<NodeId>{NodeId{99}},std::vector<NodeId>{NodeId{1},NodeId{1}}}) {
        rejects<std::invalid_argument>([&] { static_cast<void>(d.apply_availability(SimTime{10},{NodeId{3}},failed)); },
                                       "Invalid FAILED set");
    }
    require(d.task_store().tasks().at(TaskId{1}) == record && d.alive().size() == 3 && d.failed().empty() &&
            d.loads().at(NodeId{1}).assigned_work() == 4, "Invalid batch changed state");
    require(d.receive_result(SimTime{1},result(original[0])) == ResultDecision::accepted,
            "Invalid batch advanced observation time");
    static_cast<void>(d.apply_availability(SimTime{2},{NodeId{3}},{NodeId{1},NodeId{2}}));
    rejects<std::invalid_argument>([&] { d.set_alive(SimTime{3},{NodeId{1},NodeId{3}}); }, "FAILED resurrected through old API");
    rejects<std::invalid_argument>([&] { static_cast<void>(d.apply_availability(SimTime{3},{NodeId{2}},{})); },
                                   "FAILED resurrected through joint API");
    rejects<std::invalid_argument>([&] { static_cast<void>(d.apply_availability(SimTime{1},{},{})); }, "Backward availability");
    add(d,2,2); require(d.dispatch(SimTime{2})[0].node == NodeId{3}, "Invalid update changed time or RR");
    const auto no_work = d.apply_availability(SimTime{3},{},{NodeId{3}});
    require(no_work.revoked.size() == 1, "Failure without earlier assignments lost terminal status");
    d.validate_invariants();
    Dispatcher empty{{},SchedulingPolicy::least_loaded};
    const auto noop = empty.apply_availability(SimTime{0},{},{});
    require(noop.newly_failed.empty() && noop.revoked.empty() && empty.dispatch(SimTime{0}).empty(), "Empty topology");
    empty.validate_invariants();
}
}  // namespace
int main(int argc,char* argv[]) {
    try {
        if (argc != 2) { throw std::runtime_error("Expected group"); }
        const std::string_view group{argv[1]};
        if (group == "batch") { batch(); }
        else if (group == "results") { results(); }
        else if (group == "availability") { availability(); }
        else if (group == "audit") { audit(); }
        else if (group == "inputs") { inputs(); }
        else { throw std::runtime_error("Unknown group"); }
        std::cout << group << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Reconfiguration test failed: " << e.what() << '\n';
        return 1;
    }
}
