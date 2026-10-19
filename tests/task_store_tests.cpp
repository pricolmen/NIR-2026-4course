#include "telemetry/core/task_store.hpp"

#include <cmath>
#include <exception>
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
template<class Exception, class Action>
void rejects(Action action, std::string_view message) {
    try { action(); } catch (const Exception&) { return; }
    throw std::runtime_error(std::string{message});
}
TaskDescription description(int id, int arrival = 0, long double work = 4) {
    return {TaskId{id}, SimTime{arrival}, work};
}
ResultMessage result(const AssignmentCommand& command) {
    return {command.task.id, command.attempt, command.node};
}
void balanced(const TaskStore& store) {
    store.validate_invariants();
    const auto counts = store.counts();
    require(counts.arrived == counts.completed + counts.queued + counts.assigned, "Task balance");
}
struct Recorder : AuditSink {
    std::vector<AuditEvent> events;
    const TaskStore* store = nullptr;
    void on_event(const AuditEvent& event) override {
        if (store) { balanced(*store); }
        events.push_back(event);
    }
};

void queue_checks() {
    TaskStore store;
    balanced(store);
    auto input = description(3, 0, 8);
    store.arrive(SimTime{0}, input);
    input.work = 99;
    store.arrive(SimTime{0}, description(1, 0, 2));
    store.arrive(SimTime{0}, description(2, 0, 6));
    require(store.pending() == std::deque<TaskId>{TaskId{3}, TaskId{1}, TaskId{2}}, "FIFO must preserve observed order");
    require(store.counts() == TaskCounts{3, 0, 3, 0}, "Initial queued counts");
    const auto first = store.assign_next(SimTime{0}, NodeId{7});
    require(first && first->task.id == TaskId{3} && first->task.work == 8 && first->attempt == AttemptNo{1}, "First assignment");
    require(store.pending().size() == 2 && store.tasks().at(TaskId{3}).current_attempt == AttemptNo{1}, "Assignment removes FIFO entry");
    balanced(store);
    const auto second = store.assign_next(SimTime{0}, NodeId{8});
    const auto third = store.assign_next(SimTime{0}, NodeId{7});
    require(second && third && second->task.id == TaskId{1} && third->task.id == TaskId{2}, "FIFO selection order");
    require(store.counts() == TaskCounts{3, 0, 0, 3}, "All tasks assigned without waiting for results");
    const auto before = store.tasks();
    require(!store.assign_next(SimTime{0}, NodeId{9}), "Empty queue must return no command");
    require(store.tasks() == before, "Empty assignment changed history");
    balanced(store);
    TaskStore other;
    other.arrive(SimTime{0}, description(3, 0, 1));
    require(other.tasks().at(TaskId{3}).description.work == 1 && store.tasks().at(TaskId{3}).description.work == 8,
            "Store instances share mutable task state");
}

void result_checks() {
    Recorder audit;
    TaskStore store{&audit}; audit.store = &store;
    store.arrive(SimTime{0}, description(1));
    store.arrive(SimTime{0}, description(2));
    const auto first = *store.assign_next(SimTime{0}, NodeId{5});
    const auto second = *store.assign_next(SimTime{0}, NodeId{6});
    const auto unchanged = store.tasks();
    require(store.receive_result(SimTime{1}, {TaskId{99}, AttemptNo{1}, NodeId{5}}) == ResultDecision::unknown_task, "Unknown result");
    require(store.receive_result(SimTime{1}, {TaskId{1}, AttemptNo{2}, NodeId{5}}) == ResultDecision::attempt_mismatch, "Wrong attempt");
    require(store.receive_result(SimTime{1}, {TaskId{1}, AttemptNo{1}, NodeId{7}}) == ResultDecision::node_mismatch, "Wrong executor");
    require(store.tasks() == unchanged && store.counts() == TaskCounts{2, 0, 0, 2}, "Rejected result changed state");
    require(store.receive_result(SimTime{2}, result(second)) == ResultDecision::accepted, "Valid result not accepted");
    require(store.tasks().at(TaskId{2}).completed_at == SimTime{2}, "Completion time missing");
    require(store.tasks().at(TaskId{2}).attempts.back().accepted_at == SimTime{2}, "Attempt acceptance missing");
    require(!store.tasks().at(TaskId{2}).current_attempt, "Completed task still assigned");
    const auto completed_snapshot = store.tasks();
    const auto counts = store.counts();
    require(store.receive_result(SimTime{2}, result(second)) == ResultDecision::already_completed, "Duplicate result not rejected");
    require(store.tasks() == completed_snapshot && store.counts() == counts, "Duplicate changed history or counters");
    require(store.receive_result(SimTime{3}, result(first)) == ResultDecision::accepted, "Other task completion");
    require(store.counts() == TaskCounts{2, 2, 0, 0}, "Unique completed counts");
    const auto final_snapshot = store.tasks();
    rejects<std::invalid_argument>([&] { store.arrive(SimTime{3}, description(1, 3)); }, "Completed ID was reused");
    require(store.tasks() == final_snapshot, "Duplicate arrival reset a completed task");
    balanced(store);
    TaskStore zero;
    zero.arrive(SimTime{0}, description(1));
    const auto command = *zero.assign_next(SimTime{0}, NodeId{1});
    require(zero.receive_result(SimTime{0}, result(command)) == ResultDecision::accepted && zero.counts().completed == 1,
            "Optional time 0 was treated as absent");
    balanced(zero);
}

void retry_checks() {
    TaskStore store;
    store.arrive(SimTime{0}, description(1, 0, 13));
    const auto first = *store.assign_next(SimTime{0}, NodeId{1});
    store.arrive(SimTime{1}, description(2, 1, 2));
    require(store.revoke_assignment(SimTime{2}, TaskId{1}), "Assignment not revoked");
    require(store.pending() == std::deque<TaskId>{TaskId{2}, TaskId{1}}, "Returned task must append after existing FIFO");
    require(!store.revoke_assignment(SimTime{2}, TaskId{1}) && store.pending().size() == 2, "Duplicate revoke returned task twice");
    const auto revoked_snapshot = store.tasks();
    require(store.receive_result(SimTime{3}, result(first)) == ResultDecision::no_current_assignment, "Old result accepted before reassignment");
    require(store.tasks() == revoked_snapshot, "Old result modified revoked attempt");
    static_cast<void>(store.assign_next(SimTime{3}, NodeId{2}));
    const auto second = *store.assign_next(SimTime{3}, NodeId{2});
    require(second.task == first.task && second.attempt == AttemptNo{2} && second.node == NodeId{2}, "Retry lost original ID/work/arrival or number");
    const auto new_snapshot = store.tasks();
    require(store.receive_result(SimTime{4}, result(first)) == ResultDecision::attempt_mismatch, "Old result accepted after reassignment");
    require(store.tasks() == new_snapshot, "Old result modified new assignment");
    require(store.revoke_assignment(SimTime{4}, TaskId{1}), "Second revoke");
    const auto third = *store.assign_next(SimTime{4}, NodeId{1});
    require(third.attempt == AttemptNo{3} && third.task.work == 13 && third.task.arrival == SimTime{0}, "Third attempt lost original data");
    require(store.receive_result(SimTime{5}, result(second)) == ResultDecision::attempt_mismatch, "Second result accepted after third assignment");
    require(store.receive_result(SimTime{5}, result(third)) == ResultDecision::accepted, "Current retry not accepted");
    require(!store.revoke_assignment(SimTime{5}, TaskId{1}), "Completed task revoked");
    const auto& history = store.tasks().at(TaskId{1});
    require(history.attempts.size() == 3 && history.attempts[0].revoked_at == SimTime{2} &&
            history.attempts[1].revoked_at == SimTime{4} && history.attempts[2].accepted_at == SimTime{5}, "Attempt history lost");
    balanced(store);
}

void input_checks() {
    Recorder audit;
    TaskStore store{&audit};
    const auto nan = std::numeric_limits<long double>::quiet_NaN();
    const auto inf = std::numeric_limits<long double>::infinity();
    for (auto invalid : {0.0L, -1.0L, nan, inf}) {
        rejects<std::invalid_argument>([&] { store.arrive(SimTime{0}, description(1, 0, invalid)); }, "Invalid work accepted");
    }
    rejects<std::invalid_argument>([&] { store.arrive(SimTime{0}, description(1, 1)); }, "Future original arrival accepted");
    require(store.counts() == TaskCounts{} && audit.events.empty(), "Invalid arrival changed state or journal");
    store.arrive(SimTime{1}, description(1, 1));
    rejects<std::invalid_argument>([&] { store.arrive(SimTime{9}, description(1, 9)); }, "Duplicate task accepted");
    store.arrive(SimTime{2}, description(2, 2));
    const auto snapshot = store.tasks(); const auto queued = store.pending(); const auto events = audit.events.size();
    rejects<std::invalid_argument>([&] { static_cast<void>(store.assign_next(SimTime{1}, NodeId{1})); }, "Assignment time moved backwards");
    rejects<std::invalid_argument>([&] { static_cast<void>(store.receive_result(SimTime{1}, {TaskId{1}, AttemptNo{1}, NodeId{1}})); }, "Result time moved backwards");
    rejects<std::out_of_range>([&] { static_cast<void>(store.revoke_assignment(SimTime{3}, TaskId{99})); }, "Unknown revoke accepted");
    require(store.tasks() == snapshot && store.pending() == queued && audit.events.size() == events, "Invalid operation changed state");
    require(store.receive_result(SimTime{2}, {TaskId{1}, AttemptNo{1}, NodeId{1}}) == ResultDecision::no_current_assignment,
            "Unassigned queued task accepted a result");
    balanced(store);
}

struct ThrowingSink : AuditSink {
    void on_event(const AuditEvent&) override { throw std::runtime_error("Journal write failed"); }
};
struct ReentrantSink : AuditSink {
    TaskStore* store = nullptr;
    void on_event(const AuditEvent&) override { static_cast<void>(store->assign_next(SimTime{0}, NodeId{1})); }
};
struct ConditionalSink : AuditSink {
    bool fail = false;
    void on_event(const AuditEvent&) override {
        if (fail) { throw std::runtime_error("Journal write failed after commit"); }
    }
};
void audit_checks() {
    Recorder recorder;
    TaskStore store{&recorder}; recorder.store = &store;
    store.arrive(SimTime{0}, description(1));
    const auto a = *store.assign_next(SimTime{0}, NodeId{3});
    require(store.revoke_assignment(SimTime{1}, TaskId{1}), "Audit revoke");
    const auto b = *store.assign_next(SimTime{1}, NodeId{4});
    static_cast<void>(store.receive_result(SimTime{2}, result(a)));
    static_cast<void>(store.receive_result(SimTime{2}, result(b)));
    require(recorder.events.size() == 6, "Unexpected event count");
    require(std::get<TaskArrivedEvent>(recorder.events[0]).task == description(1), "Arrival audit fields");
    require(std::get<TaskAssignedEvent>(recorder.events[1]).assignment == a, "Assignment audit fields");
    require(std::get<AssignmentRevokedEvent>(recorder.events[2]).assignment == result(a), "Revoke audit identity");
    require(std::get<ResultEvent>(recorder.events[4]).decision == ResultDecision::attempt_mismatch &&
            std::get<ResultEvent>(recorder.events[5]).decision == ResultDecision::accepted, "Result audit decisions");
    ThrowingSink throwing;
    TaskStore failed{&throwing};
    rejects<std::runtime_error>([&] { failed.arrive(SimTime{0}, description(1)); }, "Sink exception was swallowed");
    balanced(failed); // Inspection after fatal run error, not resuming a run.
    require(failed.counts() == TaskCounts{1, 0, 1, 0}, "Sink saw half-committed arrival");
    ConditionalSink conditional;
    TaskStore completed{&conditional};
    completed.arrive(SimTime{0}, description(1));
    const auto command = *completed.assign_next(SimTime{0}, NodeId{1});
    conditional.fail = true;
    rejects<std::runtime_error>([&] { static_cast<void>(completed.receive_result(SimTime{1}, result(command))); },
                                "Result sink failure was swallowed");
    balanced(completed);
    require(completed.counts() == TaskCounts{1, 1, 0, 0} &&
            completed.tasks().at(TaskId{1}).attempts.back().accepted_at == SimTime{1},
            "Observer failure left half-committed completion");
    ReentrantSink reentrant;
    TaskStore guarded{&reentrant}; reentrant.store = &guarded;
    rejects<std::logic_error>([&] { guarded.arrive(SimTime{0}, description(1)); }, "Reentrant mutation was allowed");
    balanced(guarded);
}
}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) { throw std::runtime_error("Expected test group"); }
        const std::string_view group{argv[1]};
        if (group == "queue") { queue_checks(); }
        else if (group == "results") { result_checks(); }
        else if (group == "retries") { retry_checks(); }
        else if (group == "inputs") { input_checks(); }
        else if (group == "audit") { audit_checks(); }
        else { throw std::runtime_error("Unknown task test group"); }
        std::cout << group << " task checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TaskStore test failed: " << error.what() << '\n';
        return 1;
    }
}
