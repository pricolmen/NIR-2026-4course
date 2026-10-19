#include "telemetry/simulation/physical_environment.hpp"
#include "telemetry/core/dispatcher.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
using namespace telemetry;
using namespace telemetry::simulation;
using telemetry::engine::EventLoop;
static_assert(!std::is_copy_assignable_v<PhysicalTrace>);
static_assert(!std::is_move_constructible_v<PhysicalEnvironment>);
void require(bool ok,std::string_view message) {
    if (!ok) { throw std::runtime_error(std::string{message}); }
}
template<class Exception,class Action> void rejects(Action action,std::string_view message) {
    try { action(); } catch (const Exception&) { return; }
    throw std::runtime_error(std::string{message});
}
PhysicalInput input(int end,int period = 10,int node_count = 1) {
    PhysicalInput value{SimDuration{period},{},{},{},{}};
    for (int id = node_count; id >= 1; --id) {
        value.nodes.push_back({NodeId{id},1'000'000});
        for (int seq = end/period; seq >= 1; --seq) {
            value.deliveries.push_back({NodeId{id},HeartbeatSeq{seq},SimDuration{0},false});
        }
    }
    return value;
}
std::shared_ptr<const PhysicalTrace> trace(int end,PhysicalInput value) {
    return std::make_shared<const PhysicalTrace>(SimTime{end},std::move(value));
}
AssignmentCommand command(int task,int node = 1,int attempt = 1,long double work = 4) {
    return {{TaskId{task},SimTime{0},work},NodeId{node},AttemptNo{attempt}};
}
void at(EventLoop& loop,int time,engine::Phase phase,EventLoop::Action action,engine::TieKey key = {}) {
    require(loop.schedule(SimTime{time},phase,key,std::move(action)).has_value(), "Expected event within horizon");
}
struct Observer : ObservationSink,PhysicalAuditSink {
    PhysicalEnvironment* environment = nullptr;
    EventLoop* loop = nullptr;
    std::vector<std::pair<SimTime,ResultMessage>> results;
    std::vector<std::pair<SimTime,HeartbeatMessage>> heartbeats;
    std::vector<PhysicalEvent> events;
    std::function<void(SimTime,ResultMessage)> result_hook;
    bool throw_result = false;
    bool reenter = false;
    void on_event(const PhysicalEvent& event) override {
        if (environment) { environment->validate_invariants(); }
        if (reenter) { environment->submit(command(99)); }
        events.push_back(event);
    }
    void on_result(SimTime time,ResultMessage message) override {
        require(loop->last_event()->phase == phases::completion, "Result outside phase 2");
        if (environment) { environment->validate_invariants(); }
        results.emplace_back(time,message);
        if (result_hook) { result_hook(time,message); }
        if (throw_result) { throw std::runtime_error("Result sink failed"); }
    }
    void on_heartbeat(SimTime time,HeartbeatMessage message) override {
        require(loop->last_event()->phase == phases::heartbeat_receive, "Receipt outside phase 4");
        heartbeats.emplace_back(time,message);
    }
};
template<class Event> std::vector<Event> events(const Observer& observer) {
    std::vector<Event> result;
    for (const auto& entry : observer.events) {
        if (const auto* value = std::get_if<Event>(&entry)) { result.push_back(*value); }
    }
    return result;
}
void fifo() {
    auto shared = trace(20,input(20,10,2));
    EventLoop loop{SimTime{20}}; Observer observer; observer.loop = &loop;
    PhysicalEnvironment env{loop,shared,observer,&observer}; observer.environment = &env;
    env.initialize();
    at(loop,0,phases::dispatch,[&](EventLoop&) {
        env.submit(command(1)); env.submit(command(2,1,1,2));
        env.submit(command(3,2,1,1)); env.submit(command(4,2,1,0.25L));
    });
    observer.result_hook = [&](SimTime time,ResultMessage message) {
        if (message.task == TaskId{1}) {
            require(time == SimTime{4} && !env.nodes().at(NodeId{1}).running &&
                    !env.attempts().at({TaskId{2},AttemptNo{1}}).started_at,
                    "Next old attempt started before immediate result observation");
        }
    };
    require(loop.run().reached == SimTime{20}, "Physical run horizon");
    const auto& attempts = env.attempts();
    require(attempts.at({TaskId{1},AttemptNo{1}}).started_at == SimTime{0} &&
            attempts.at({TaskId{1},AttemptNo{1}}).finished_at == SimTime{4} &&
            attempts.at({TaskId{2},AttemptNo{1}}).started_at == SimTime{4} &&
            attempts.at({TaskId{2},AttemptNo{1}}).finished_at == SimTime{6}, "Sequential FIFO/nonpreemption");
    require(attempts.at({TaskId{3},AttemptNo{1}}).finished_at == SimTime{1} &&
            attempts.at({TaskId{4},AttemptNo{1}}).finished_at == SimTime{2}, "Independent node and positive sub-tick duration");
    require(observer.results.size() == 4 && observer.results[0].second.task == TaskId{3} &&
            observer.results[3].second.task == TaskId{2}, "Result order");
    const auto sent = events<PhysicalHeartbeatSent>(observer);
    require(sent.size() == 4 && sent[0].at == SimTime{10} && sent[0].message.node == NodeId{1} &&
            sent[1].message.node == NodeId{2} && sent[2].at == SimTime{20} &&
            sent[2].message.sequence == HeartbeatSeq{2}, "Periodic sends at H,2H and stable node order");
    require(observer.heartbeats.size() == 4 && observer.heartbeats[0].first == SimTime{10}, "Zero delay receipt");
    env.validate_invariants();
    // Same-time completions compare node, task and attempt, not insertion order.
    EventLoop equal{SimTime{4}}; Observer equal_observer; equal_observer.loop = &equal;
    PhysicalEnvironment equal_env{equal,trace(4,input(4,10,2)),equal_observer}; equal_env.initialize();
    at(equal,0,phases::dispatch,[&](EventLoop&) { equal_env.submit(command(2,2)); equal_env.submit(command(1,1)); });
    static_cast<void>(equal.run());
    require(equal_observer.results[0].second.node == NodeId{1}, "Coincident completions ignore canonical node order");
}
void crash() {
    auto value = input(12,4);
    value.crashes.push_back({NodeId{1},SimTime{4}});
    EventLoop loop{SimTime{12}}; Observer observer; observer.loop = &loop;
    PhysicalEnvironment env{loop,trace(12,std::move(value)),observer,&observer}; observer.environment = &env;
    env.initialize();
    at(loop,0,phases::dispatch,[&](EventLoop&) { env.submit(command(1)); env.submit(command(2)); });
    at(loop,5,phases::dispatch,[&](EventLoop&) { env.submit(command(3)); }); // Still ALIVE in an uninformed core.
    static_cast<void>(loop.run());
    require(observer.results.empty() && observer.heartbeats.empty(), "Crash at completion/send did not win phase 1");
    const auto& first = env.attempts().at({TaskId{1},AttemptNo{1}});
    require(first.interrupted_at == SimTime{4} && !first.finished_at && first.completion_token == 1 &&
            env.nodes().at(NodeId{1}).completion_token == 2, "Crash did not invalidate separate completion token");
    require(!env.attempts().at({TaskId{2},AttemptNo{1}}).started_at &&
            !env.attempts().at({TaskId{3},AttemptNo{1}}).started_at && env.nodes().at(NodeId{1}).waiting.size() == 2,
            "Old or post-crash queued command started");
    const auto skipped = events<PhysicalCompletionSkipped>(observer);
    require(skipped.size() == 1 && skipped[0].reason == CompletionSkipReason::crashed &&
            skipped[0].scheduled_token != skipped[0].current_token && events<PhysicalSendSkipped>(observer).size() == 1,
            "Obsolete physical events lack skip diagnostics");
    env.validate_invariants();
    auto zero_input = input(4,2); zero_input.crashes.push_back({NodeId{1},SimTime{0}});
    EventLoop zero{SimTime{4}}; Observer zero_observer; zero_observer.loop = &zero;
    PhysicalEnvironment zero_env{zero,trace(4,std::move(zero_input)),zero_observer}; zero_env.initialize();
    at(zero,0,phases::dispatch,[&](EventLoop&) { zero_env.submit(command(1)); });
    static_cast<void>(zero.run());
    require(zero_env.nodes().at(NodeId{1}).crashed && !zero_env.attempts().begin()->second.started_at,
            "Crash at zero treated as absent");
}
void channel() {
    auto value = input(30,5);
    for (auto& record : value.deliveries) {
        if (record.sequence == HeartbeatSeq{1}) { record.delay = SimDuration{3}; } // receipt 8, before blackout
        if (record.sequence == HeartbeatSeq{2}) { record.delay = SimDuration{5}; } // receipt 15, start -> lost
        if (record.sequence == HeartbeatSeq{3}) { record.delay = SimDuration{10}; } // receipt 25, end -> passes
        if (record.sequence == HeartbeatSeq{6}) { record.lost = true; }
    }
    value.blackouts = {{NodeId{1},SimTime{20},SimTime{25}},{NodeId{1},SimTime{15},SimTime{21}}};
    const auto shared = trace(30,std::move(value));
    require(shared->blackouts().size() == 1 && shared->blocked(NodeId{1},SimTime{15}) &&
            !shared->blocked(NodeId{1},SimTime{25}), "Overlapping intervals not merged as a half-open union");
    EventLoop loop{SimTime{30}}; Observer observer; observer.loop = &loop;
    PhysicalEnvironment env{loop,shared,observer,&observer}; env.initialize(); static_cast<void>(loop.run());
    require(observer.heartbeats.size() == 3 && observer.heartbeats[0].second.sequence == HeartbeatSeq{1} &&
            observer.heartbeats[1].first == SimTime{25} && observer.heartbeats[1].second.sequence == HeartbeatSeq{3} &&
            observer.heartbeats[2].second.sequence == HeartbeatSeq{5}, "Blackout edges, lost trace or deferred delivery");
    const auto sent = events<PhysicalHeartbeatSent>(observer);
    require(sent.size() == 6 && sent[1].disposition == DeliveryDisposition::blackout &&
            sent[1].receive_at == SimTime{15} && sent.back().disposition == DeliveryDisposition::trace_lost,
            "Loss reasons/planned receive diagnostics");
    auto late_input = input(20,5);
    late_input.crashes.push_back({NodeId{1},SimTime{7}});
    for (auto& record : late_input.deliveries) { record.delay = SimDuration{4}; }
    EventLoop late{SimTime{20}}; Observer late_observer; late_observer.loop = &late;
    PhysicalEnvironment late_env{late,trace(20,std::move(late_input)),late_observer,&late_observer}; late_env.initialize();
    static_cast<void>(late.run());
    require(late_observer.heartbeats.size() == 1 && late_observer.heartbeats[0].first == SimTime{9} &&
            late_env.nodes().at(NodeId{1}).sent_heartbeats == 1, "In-flight heartbeat was cancelled by crash");
    auto reordered_input = input(20,5);
    for (auto& record : reordered_input.deliveries) {
        if (record.sequence == HeartbeatSeq{1}) { record.delay = SimDuration{11}; }
    }
    EventLoop reordered{SimTime{20}}; Observer reordered_observer; reordered_observer.loop = &reordered;
    PhysicalEnvironment reordered_env{reordered,trace(20,std::move(reordered_input)),reordered_observer};
    reordered_env.initialize(); static_cast<void>(reordered.run());
    require(reordered_observer.heartbeats[0].second.sequence == HeartbeatSeq{2} &&
            reordered_observer.heartbeats[2].second.sequence == HeartbeatSeq{1},
            "Physical channel filtered reordered sequences instead of leaving admissibility to core");
}
void exclusion() {
    using namespace telemetry::core;
    for (const bool later_crash : {false,true}) {
        auto value = input(10,1,2);
        if (later_crash) { value.crashes.push_back({NodeId{1},SimTime{5}}); }
        auto shared = trace(10,std::move(value));
        EventLoop loop{SimTime{10}}; Observer observer; observer.loop = &loop;
        Dispatcher core{{{NodeId{1},1'000'000},{NodeId{2},1'000'000}},SchedulingPolicy::round_robin};
        core.set_alive(SimTime{0},{NodeId{1}});
        PhysicalEnvironment env{loop,shared,observer,&observer}; observer.environment = &env;
        env.initialize();
        std::vector<ResultDecision> decisions;
        observer.result_hook = [&](SimTime time,ResultMessage message) { decisions.push_back(core.receive_result(time,message)); };
        at(loop,0,phases::arrival,[&](EventLoop&) {
            core.arrive(SimTime{0},command(1).task); core.arrive(SimTime{0},command(2,1,1,2).task);
        });
        at(loop,0,phases::dispatch,[&](EventLoop&) { for (const auto& a : core.dispatch(loop.now())) { env.submit(a); } });
        at(loop,2,phases::reconfiguration,[&](EventLoop&) {
            const auto returned = core.apply_availability(loop.now(),{NodeId{2}},{NodeId{1}});
            require(returned.revoked.size() == 2 && env.nodes().at(NodeId{1}).running &&
                    env.nodes().at(NodeId{1}).waiting.size() == 1, "False FAILED stopped/cleared physical old work");
        });
        at(loop,2,phases::dispatch,[&](EventLoop&) { for (const auto& a : core.dispatch(loop.now())) { env.submit(a); } });
        static_cast<void>(loop.run());
        const auto& old1 = env.attempts().at({TaskId{1},AttemptNo{1}});
        const auto& old2 = env.attempts().at({TaskId{2},AttemptNo{1}});
        require(old1.finished_at == SimTime{4} && old2.started_at == SimTime{4}, "False FAILED interrupted current/FIFO attempt");
        if (later_crash) {
            require(!old2.finished_at && old2.interrupted_at == SimTime{5} &&
                    env.nodes().at(NodeId{1}).sent_heartbeats == 4, "Later actual crash did not stop old work and sends");
            require(events<PhysicalCompletionSkipped>(observer).size() == 1, "Later crash stale completion missing");
        } else {
            require(old2.finished_at == SimTime{6} && env.nodes().at(NodeId{1}).sent_heartbeats == 10 &&
                    decisions.size() == 4 && decisions[1] == ResultDecision::attempt_mismatch,
                    "Excluded live node failed to continue old local work/heartbeat");
        }
        require(decisions.front() == ResultDecision::attempt_mismatch && core.task_store().counts().completed == 2,
                "Stale physical result accepted or retry failed");
        require(env.attempts().at({TaskId{1},AttemptNo{2}}).duration == SimDuration{4} &&
                env.attempts().at({TaskId{1},AttemptNo{2}}).completion_token == 1,
                "Retry used remaining work or conflated attempt number with physical token");
        core.validate_invariants(); env.validate_invariants();
    }
    // No replacement executor: logical revocation still leaves old physical
    // work alive, and its results must fail even with no current assignment.
    EventLoop none{SimTime{8}}; Observer no_retry; no_retry.loop = &none;
    Dispatcher revoked{{{NodeId{1},1'000'000}},SchedulingPolicy::round_robin};
    PhysicalEnvironment continued{none,trace(8,input(8,2)),no_retry}; continued.initialize();
    std::vector<ResultDecision> rejected;
    no_retry.result_hook = [&](SimTime time,ResultMessage message) { rejected.push_back(revoked.receive_result(time,message)); };
    at(none,0,phases::arrival,[&](EventLoop&) {
        revoked.arrive(SimTime{0},command(1).task); revoked.arrive(SimTime{0},command(2,1,1,2).task);
    });
    at(none,0,phases::dispatch,[&](EventLoop&) { for (const auto& a : revoked.dispatch(none.now())) { continued.submit(a); } });
    at(none,2,phases::reconfiguration,[&](EventLoop&) { static_cast<void>(revoked.apply_availability(none.now(),{},{NodeId{1}})); });
    static_cast<void>(none.run());
    require(rejected == std::vector<ResultDecision>{ResultDecision::no_current_assignment,ResultDecision::no_current_assignment} &&
            revoked.task_store().counts() == TaskCounts{2,0,2,0} && no_retry.heartbeats.size() == 4,
            "False FAILED without reassignment stopped old work or admitted its result");
    continued.validate_invariants(); revoked.validate_invariants();
}

void horizon() {
    EventLoop empty{SimTime{0}}; Observer empty_observer; empty_observer.loop = &empty;
    PhysicalInput empty_input{SimDuration{1},{},{},{},{}};
    PhysicalEnvironment empty_env{empty,trace(0,std::move(empty_input)),empty_observer};
    empty_env.initialize();
    require(empty.run().executed == 0 && empty_env.nodes().empty(), "Empty physical world at zero horizon");
    empty_env.validate_invariants();
    EventLoop exact{SimTime{4}}; Observer observer; observer.loop = &exact;
    PhysicalEnvironment env{exact,trace(4,input(4)),observer,&observer}; env.initialize();
    at(exact,0,phases::dispatch,[&](EventLoop&) { env.submit(command(1)); env.submit(command(2)); });
    static_cast<void>(exact.run());
    require(observer.results.size() == 1 && observer.results[0].first == SimTime{4} &&
            env.attempts().at({TaskId{2},AttemptNo{1}}).started_at == SimTime{4} &&
            !env.attempts().at({TaskId{2},AttemptNo{1}}).completion_due, "Inclusive completion/start at horizon");
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    PhysicalInput value{SimDuration{max},{{NodeId{1},1'000'000}},{},{},{{NodeId{1},HeartbeatSeq{1},SimDuration{max},false}}};
    const auto shared = std::make_shared<const PhysicalTrace>(SimTime{max},std::move(value));
    EventLoop large{SimTime{max}}; Observer large_observer; large_observer.loop = &large;
    PhysicalEnvironment large_env{large,shared,large_observer,&large_observer}; large_env.initialize();
    static_cast<void>(large.schedule(SimTime{max-1},phases::dispatch,{},[&](EventLoop&) { large_env.submit(command(1)); }));
    static_cast<void>(large.run());
    require(large_observer.results.empty() && large_observer.heartbeats.empty() &&
            large_env.attempts().begin()->second.started_at == SimTime{max-1} &&
            !large_env.attempts().begin()->second.completion_due, "Completion/delivery addition overflow near horizon");
    require(events<PhysicalHeartbeatSent>(large_observer)[0].disposition == DeliveryDisposition::beyond_horizon,
            "Beyond-horizon delivery diagnostic");
    large_env.validate_invariants();
    // Reusing the very same immutable input creates independent state.
    const auto reused = trace(12,input(12,3));
    const auto run = [&]() {
        EventLoop loop{SimTime{12}}; Observer recorder; recorder.loop = &loop;
        PhysicalEnvironment independent{loop,reused,recorder}; independent.initialize();
        at(loop,0,phases::dispatch,[&](EventLoop&) { independent.submit(command(1)); });
        static_cast<void>(loop.run());
        return std::pair{recorder.results,recorder.heartbeats};
    };
    require(run() == run() && reused->deliveries().size() == 4, "Variant state leaked through shared physical trace");
}
void inputs() {
    const auto invalid_trace = [](PhysicalInput value) {
        rejects<std::invalid_argument>([&] { static_cast<void>(trace(10,std::move(value))); }, "Invalid physical input accepted");
    };
    rejects<std::invalid_argument>([] {
        PhysicalInput incomplete{SimDuration{1},{{NodeId{1},1}},{},{},{}};
        PhysicalTrace huge{SimTime{std::numeric_limits<std::int64_t>::max()},std::move(incomplete)};
    }, "Huge missing trace should fail without walking every potential send");
    auto value = input(10); value.heartbeat_period = SimDuration{0}; invalid_trace(std::move(value));
    value = input(10); value.nodes.push_back(value.nodes.front()); invalid_trace(std::move(value));
    for (const auto p : {0.0L,-1.0L,std::numeric_limits<long double>::quiet_NaN(),std::numeric_limits<long double>::infinity()}) {
        value = input(10); value.nodes[0].performance = p; invalid_trace(std::move(value));
    }
    value = input(10); value.deliveries.clear(); invalid_trace(std::move(value));
    value = input(10); value.deliveries.push_back(value.deliveries.front()); invalid_trace(std::move(value));
    value = input(10); value.deliveries[0].sequence = HeartbeatSeq{2}; invalid_trace(std::move(value));
    value = input(10); value.deliveries[0].node = NodeId{2}; invalid_trace(std::move(value));
    value = input(10); value.crashes = {{NodeId{1},SimTime{0}},{NodeId{1},SimTime{1}}}; invalid_trace(std::move(value));
    value = input(10); value.crashes = {{NodeId{1},SimTime{11}}}; invalid_trace(std::move(value));
    value = input(10); value.crashes = {{NodeId{2},SimTime{0}}}; invalid_trace(std::move(value));
    value = input(10); value.blackouts = {{NodeId{1},SimTime{5},SimTime{5}}}; invalid_trace(std::move(value));
    value = input(10); value.blackouts = {{NodeId{2},SimTime{0},SimTime{1}}}; invalid_trace(std::move(value));
    // Blackout intervals may extend beyond horizon; only start < end is required.
    value = input(10); value.blackouts = {{NodeId{1},SimTime{100},SimTime{200}}};
    require(trace(10,std::move(value))->blackouts().size() == 1, "Blackout unnecessarily clipped/rejected");
    EventLoop loop{SimTime{10}}; Observer observer; observer.loop = &loop;
    const auto shared = trace(10,input(10));
    rejects<std::invalid_argument>([&] { PhysicalEnvironment bad{loop,nullptr,observer}; }, "Null trace accepted");
    rejects<std::invalid_argument>([&] { PhysicalEnvironment bad{loop,trace(9,input(9)),observer}; }, "Horizon mismatch accepted");
    PhysicalEnvironment env{loop,shared,observer,&observer}; observer.environment = &env;
    rejects<std::logic_error>([&] { env.submit(command(1)); }, "Submit before initialization accepted");
    env.initialize();
    rejects<std::logic_error>([&] { env.initialize(); }, "Repeated initialization accepted");
    rejects<std::logic_error>([&] { env.submit(command(1)); }, "Submit outside running phase accepted");
    at(loop,0,phases::arrival,[&](EventLoop&) {
        rejects<std::logic_error>([&] { env.submit(command(1)); }, "Submit outside dispatch phase accepted");
    });
    at(loop,0,phases::dispatch,[&](EventLoop&) {
        rejects<std::out_of_range>([&] { env.submit(command(1,99)); }, "Unknown executor accepted");
        auto future = command(1); future.task.arrival = SimTime{1};
        rejects<std::invalid_argument>([&] { env.submit(future); }, "Future task accepted");
        rejects<std::invalid_argument>([&] { env.submit(command(1,1,1,0)); }, "Zero work accepted");
        env.submit(command(1));
        rejects<std::invalid_argument>([&] { env.submit(command(1)); }, "Duplicate attempt accepted");
        env.submit(command(2));
        require(env.attempts().size() == 2 && env.nodes().at(NodeId{1}).waiting.size() == 1, "Invalid submit mutated physical state");
    });
    static_cast<void>(loop.run()); env.validate_invariants();
    require(observer.results.size() == 2, "Valid work after rejected commands failed");
    rejects<std::invalid_argument>([&] { PhysicalEnvironment bad{loop,shared,observer}; }, "Used event loop accepted");
    EventLoop failure{SimTime{10}}; Observer failing; failing.loop = &failure; failing.throw_result = true;
    PhysicalEnvironment stopped{failure,shared,failing,&failing}; failing.environment = &stopped; stopped.initialize();
    at(failure,0,phases::dispatch,[&](EventLoop&) { stopped.submit(command(1)); stopped.submit(command(2)); });
    rejects<std::runtime_error>([&] { static_cast<void>(failure.run()); }, "Sink failure swallowed");
    require(failure.state() == engine::RunState::failed && stopped.attempts().at({TaskId{1},AttemptNo{1}}).finished_at &&
            !stopped.attempts().at({TaskId{2},AttemptNo{1}}).started_at, "Sink error ran later physical work");
    stopped.validate_invariants();
    EventLoop recursive{SimTime{10}}; Observer guarded; guarded.loop = &recursive; guarded.reenter = true;
    PhysicalEnvironment guard{recursive,shared,guarded,&guarded}; guarded.environment = &guard; guard.initialize();
    at(recursive,0,phases::dispatch,[&](EventLoop&) { guard.submit(command(1)); });
    rejects<std::logic_error>([&] { static_cast<void>(recursive.run()); }, "Reentrant mutation accepted");
    guard.validate_invariants();
}
}  // namespace
int main(int argc,char* argv[]) {
    try {
        if (argc != 2) { throw std::runtime_error("Expected group"); }
        const std::string_view group{argv[1]};
        if (group == "fifo") { fifo(); }
        else if (group == "crash") { crash(); }
        else if (group == "channel") { channel(); }
        else if (group == "exclusion") { exclusion(); }
        else if (group == "horizon") { horizon(); }
        else if (group == "inputs") { inputs(); }
        else { throw std::runtime_error("Unknown group"); }
        std::cout << group << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Physical environment test failed: " << e.what() << '\n';
        return 1;
    }
}
