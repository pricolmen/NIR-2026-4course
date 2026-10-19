#include "telemetry/core/dispatcher.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace telemetry;
using namespace telemetry::core;
void require(bool condition, std::string_view message) {
    if (!condition) { throw std::runtime_error(std::string{message}); }
}
template<class Exception, class Action> void rejects(Action action, std::string_view message) {
    try { action(); } catch (const Exception&) { return; }
    throw std::runtime_error(std::string{message});
}
std::vector<NodeDefinition> nodes(long double second = 1) { return {{NodeId{2}, second}, {NodeId{1}, 1}}; }
ResultMessage result(const AssignmentCommand& a) { return {a.task.id, a.attempt, a.node}; }
void add(Dispatcher& d, int id, long double work, int at = 0) {
    d.arrive(SimTime{at}, {TaskId{id}, SimTime{at}, work});
}
std::vector<NodeId> choices(const std::vector<AssignmentCommand>& commands) {
    std::vector<NodeId> ids;
    for (const auto& c : commands) { ids.push_back(c.node); }
    return ids;
}

void rr_checks() {
    RoundRobin rr{{NodeId{3}, NodeId{1}, NodeId{2}}};
    for (auto expected : {1, 2, 3, 1}) {
        require(rr.peek() == NodeId{expected}, "RR cycle or initial minimum"); rr.advance();
    }
    require(rr.peek() == NodeId{2}, "Next ID lost");
    rr.update_alive({NodeId{1}, NodeId{3}});
    require(rr.peek() == NodeId{3}, "Removed next must use cyclic successor");
    rr.update_alive({NodeId{1}, NodeId{2}, NodeId{3}});
    require(rr.peek() == NodeId{3}, "Returning SUSPECT reset RR position");
    rr.update_alive({}); require(!rr.peek(), "Empty RR selected a node");
    rr.update_alive({NodeId{1}, NodeId{2}});
    require(rr.peek() == NodeId{1}, "Empty-set anchor must wrap from removed 3 to 1");
    RoundRobin simultaneous{{NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}}};
    simultaneous.advance();
    simultaneous.update_alive({NodeId{4}, NodeId{1}});
    require(simultaneous.peek() == NodeId{4}, "Joint removal of next and successor");
    simultaneous.update_alive({NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}});
    require(simultaneous.peek() == NodeId{4}, "Joint return reset prepared position");
    rejects<std::invalid_argument>([&] { rr.update_alive({NodeId{99}}); }, "Unknown ALIVE accepted");
    require(rr.peek() == NodeId{1}, "Invalid update changed RR");
    rejects<std::invalid_argument>([&] { rr.update_alive({NodeId{1}, NodeId{1}}); }, "Duplicate ALIVE accepted");
    RoundRobin empty{{}}; require(!empty.peek(), "Empty topology");
    rejects<std::logic_error>([&] { empty.advance(); }, "Empty RR advanced");
}

void ll_checks() {
    LoadLedger loads{nodes(2)};
    const std::vector<NodeId> alive{NodeId{2}, NodeId{1}};
    require(LeastLoaded::select(alive, loads) == NodeId{1}, "LL tie must choose minimum ID independent of iteration");
    loads.add(NodeId{1}, 4); loads.add(NodeId{2}, 8);
    require(loads.at(NodeId{1}).load() == 4 && loads.at(NodeId{2}).load() == 4, "Performance units");
    require(LeastLoaded::select(alive, loads) == NodeId{1}, "Equal normalized loads");
    loads.remove(NodeId{2}, 8);
    const std::vector<NodeId> only_one{NodeId{1}};
    require(LeastLoaded::select(only_one, loads) == NodeId{1}, "LL selected a less loaded non-ALIVE node");
    require(LeastLoaded::select(alive, loads) == NodeId{2}, "Performance affects selection");
    require(!LeastLoaded::select({}, loads), "LL empty set");
    LoadLedger close{nodes()};
    close.add(NodeId{1}, std::nextafter(1.0L, 2.0L)); close.add(NodeId{2}, 1);
    require(LeastLoaded::select(alive, close) == NodeId{2}, "Epsilon changed unequal LL loads into a tie");
    Dispatcher ll{nodes(), SchedulingPolicy::least_loaded};
    for (int i = 1; i <= 4; ++i) { add(ll, i, i % 2 ? 4 : 2); }
    require(choices(ll.dispatch(SimTime{0})) == std::vector<NodeId>{NodeId{1},NodeId{2},NodeId{2},NodeId{1}},
            "Assignments at one timestamp did not see updated loads");
    const auto work = ll.loads().at(NodeId{1}).assigned_work();
    static_cast<void>(ll.dispatch(SimTime{100}));
    require(ll.loads().at(NodeId{1}).assigned_work() == work, "Elapsed time reduced full executing work");
    ll.validate_invariants();
    Dispatcher faster{nodes(2), SchedulingPolicy::least_loaded};
    for (int i = 1; i <= 4; ++i) { add(faster, i, i % 2 ? 4 : 2); }
    require(choices(faster.dispatch(SimTime{0})) == std::vector<NodeId>{NodeId{1},NodeId{2},NodeId{2},NodeId{2}},
            "Different performance did not affect LL decisions");
    faster.validate_invariants();
}

struct Observer : AuditSink {
    const Dispatcher* dispatcher = nullptr;
    bool fail = false;
    void on_event(const AuditEvent&) override {
        if (dispatcher) { dispatcher->validate_invariants(); }
        if (fail) { throw std::runtime_error("Journal error"); }
    }
};
struct ReentrantObserver : AuditSink {
    Dispatcher* dispatcher = nullptr;
    void on_event(const AuditEvent&) override { dispatcher->set_alive(SimTime{0}, {}); }
};
void integration_checks() {
    Observer observer;
    Dispatcher d{nodes(), SchedulingPolicy::round_robin, &observer}; observer.dispatcher = &d;
    require(d.dispatch(SimTime{0}).empty(), "Empty FIFO created an assignment");
    for (int i = 1; i <= 4; ++i) { add(d, i, i % 2 ? 4 : 2); }
    const auto assigned = d.dispatch(SimTime{0});
    require(choices(assigned) == std::vector<NodeId>{NodeId{1},NodeId{2},NodeId{1},NodeId{2}}, "Baseline RR choices");
    require(d.loads().at(NodeId{1}).assigned_work() == 8 && d.loads().at(NodeId{2}).assigned_work() == 4, "Full work missing");
    require(d.receive_result(SimTime{1}, {TaskId{99},AttemptNo{1},NodeId{99}}) == ResultDecision::unknown_task,
            "Unknown result eagerly touched nonexistent node load");
    require(d.receive_result(SimTime{1}, {TaskId{1},AttemptNo{1},NodeId{99}}) == ResultDecision::node_mismatch,
            "Wrong executor eagerly touched nonexistent node load");
    require(d.receive_result(SimTime{1}, result(assigned[0])) == ResultDecision::accepted, "Valid result");
    require(d.loads().at(NodeId{1}).assigned_work() == 4, "Accepted result did not remove exactly once");
    static_cast<void>(d.receive_result(SimTime{1}, result(assigned[0])));
    require(d.loads().at(NodeId{1}).assigned_work() == 4, "Duplicate removed load twice");
    require(d.revoke_assignment(SimTime{2}, TaskId{3}), "Revoke failed");
    require(!d.revoke_assignment(SimTime{2}, TaskId{3}), "Duplicate revoke");
    require(d.loads().at(NodeId{1}).assigned_work() == 0 && d.loads().at(NodeId{1}).assignments == 0, "Empty node must be exact zero");
    require(d.receive_result(SimTime{2}, result(assigned[2])) == ResultDecision::no_current_assignment, "Old result before reassignment");
    d.set_alive(SimTime{2}, {NodeId{2}});
    const auto retry = d.dispatch(SimTime{2});
    require(retry.size() == 1 && retry[0].attempt == AttemptNo{2} && retry[0].task.work == 4, "Retry full work");
    require(d.loads().at(NodeId{2}).assigned_work() == 8, "Retry missing from new load");
    static_cast<void>(d.receive_result(SimTime{3}, result(assigned[2])));
    require(d.loads().at(NodeId{2}).assigned_work() == 8, "Old result decreased new load");
    require(d.receive_result(SimTime{3}, result(retry[0])) == ResultDecision::accepted, "Current retry result");
    static_cast<void>(d.receive_result(SimTime{4}, result(assigned[1])));
    static_cast<void>(d.receive_result(SimTime{4}, result(assigned[3])));
    require(d.loads().at(NodeId{2}).assigned_work() == 0 && d.task_store().counts().completed == 4, "Final balance and zero");
    d.validate_invariants();
    Observer failing;
    Dispatcher error{nodes(), SchedulingPolicy::round_robin, &failing}; failing.dispatcher = &error;
    add(error, 1, 3); failing.fail = true;
    rejects<std::runtime_error>([&] { static_cast<void>(error.dispatch(SimTime{0})); }, "Sink error swallowed");
    error.validate_invariants();
    require(error.loads().at(NodeId{1}).assigned_work() == 3, "Observer saw assignment before load update");
    ReentrantObserver reentrant;
    Dispatcher guarded{nodes(), SchedulingPolicy::round_robin, &reentrant}; reentrant.dispatcher = &guarded;
    rejects<std::logic_error>([&] { add(guarded, 1, 1); }, "Callback mutated availability during dispatch operation");
    require(guarded.alive().size() == 2, "Reentrant callback changed ALIVE");
    guarded.validate_invariants();
}

void availability_checks() {
    Dispatcher d{nodes(), SchedulingPolicy::round_robin};
    add(d, 1, 4); const auto first = d.dispatch(SimTime{0});
    d.set_alive(SimTime{1}, {NodeId{2}}); // SUSPECT does not revoke old work.
    require(d.loads().at(NodeId{1}).assigned_work() == 4, "Unavailable node lost existing work");
    add(d, 2, 2, 1); require(d.dispatch(SimTime{1})[0].node == NodeId{2}, "New assignment to non-ALIVE");
    require(d.receive_result(SimTime{1}, result(first[0])) == ResultDecision::accepted,
            "Current result from a non-ALIVE node was rejected");
    d.set_alive(SimTime{2}, {}); add(d, 3, 1, 2);
    require(d.dispatch(SimTime{2}).empty() && d.task_store().pending().size() == 1, "Empty ALIVE lost queued work");
    d.set_alive(SimTime{3}, {NodeId{2},NodeId{1}});
    require(d.dispatch(SimTime{3})[0].node == NodeId{2}, "Empty interval lost successor anchor");
    require(d.receive_result(SimTime{4}, result(first[0])) == ResultDecision::already_completed, "Duplicate after availability change");
    d.validate_invariants();
    Dispatcher invalid{nodes(), SchedulingPolicy::round_robin};
    rejects<std::invalid_argument>([&] { invalid.set_alive(SimTime{9}, {NodeId{99}}); }, "Unknown eligibility");
    rejects<std::invalid_argument>([&] { invalid.set_alive(SimTime{9}, {NodeId{1},NodeId{1}}); }, "Duplicate eligibility");
    add(invalid, 1, 1); require(invalid.dispatch(SimTime{0})[0].node == NodeId{1}, "Invalid availability changed time/position");
    invalid.set_alive(SimTime{5}, {});
    rejects<std::invalid_argument>([&] { static_cast<void>(invalid.dispatch(SimTime{4})); }, "Availability time not included in monotonicity");
}

void numeric_checks() {
    Dispatcher cancellation{{{NodeId{1},1}}, SchedulingPolicy::round_robin};
    add(cancellation, 1, 1e20L); add(cancellation, 2, 1);
    const auto commands = cancellation.dispatch(SimTime{0});
    static_cast<void>(cancellation.receive_result(SimTime{1}, result(commands[0])));
    require(cancellation.loads().at(NodeId{1}).assigned_work() == 1, "Large removal erased small outstanding work");
    cancellation.validate_invariants();
    static_cast<void>(cancellation.receive_result(SimTime{1}, result(commands[1])));
    require(cancellation.loads().at(NodeId{1}).assigned_work() == 0, "Compensated empty node not zero");
    const auto max = std::numeric_limits<long double>::max();
    const auto min = std::numeric_limits<long double>::min();
    for (auto bad : {0.0L,-1.0L,max+max,std::numeric_limits<long double>::quiet_NaN()}) {
        rejects<std::invalid_argument>([&] { LoadLedger invalid{nodes(bad)}; }, "Invalid performance accepted");
    }
    rejects<std::invalid_argument>([] { LoadLedger duplicate{{{NodeId{1},1},{NodeId{1},2}}}; }, "Duplicate node catalog");
    LoadLedger tiny{{{NodeId{1},max}}};
    rejects<std::underflow_error>([&] { tiny.add(NodeId{1},min); }, "Silent load underflow");
    require(tiny.at(NodeId{1}).assignments == 0, "Invalid arithmetic changed counter");
    rejects<std::logic_error>([&] { tiny.remove(NodeId{1},1); }, "Empty load removed twice");
    Dispatcher overflow{{{NodeId{1},1}}, SchedulingPolicy::round_robin};
    add(overflow, 1, max); add(overflow, 2, max);
    rejects<std::overflow_error>([&] { static_cast<void>(overflow.dispatch(SimTime{0})); }, "Load overflow not rejected");
    require(overflow.task_store().counts() == TaskCounts{2,0,1,1}, "Overflow committed the failing second assignment");
    overflow.validate_invariants();
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) { throw std::runtime_error("Expected scheduling group"); }
        const std::string_view group{argv[1]};
        if (group=="rr") { rr_checks(); }
        else if (group=="ll") { ll_checks(); }
        else if (group=="integration") { integration_checks(); }
        else if (group=="availability") { availability_checks(); }
        else if (group=="numeric") { numeric_checks(); }
        else { throw std::runtime_error("Unknown scheduling group"); }
        std::cout << group << " scheduling checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
