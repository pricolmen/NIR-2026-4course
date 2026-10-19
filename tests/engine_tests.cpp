#include "telemetry/engine/event_loop.hpp"

#include <concepts>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace telemetry;
using namespace telemetry::engine;
static_assert(!std::constructible_from<Phase,bool>);
static_assert(!std::constructible_from<Phase,double>);
static_assert(!std::copy_constructible<EventLoop>);
static_assert(!std::move_constructible<EventLoop>);
static_assert(!std::constructible_from<EventLoop,SimTime,bool>);
static_assert(!std::constructible_from<EventLoop,SimTime,double>);
void require(bool ok,std::string_view message) {
    if (!ok) { throw std::runtime_error(std::string{message}); }
}
template<class Exception,class Action> void rejects(Action action,std::string_view message) {
    try { action(); } catch (const Exception&) { return; }
    throw std::runtime_error(std::string{message});
}
void enqueue(EventLoop& loop, SimTime at, Phase phase, TieKey key, EventLoop::Action action) {
    require(loop.schedule(at,phase,key,std::move(action)).has_value(), "Expected event admission");
}
void ordering() {
    EventLoop loop{SimTime{10}};
    std::vector<int> seen;
    const auto add = [&](int tag,int time,int phase,TieKey key) {
        enqueue(loop,SimTime{time},Phase{phase},key,[&,tag,time,phase,key](EventLoop& running) {
            require(running.now() == SimTime{time} && running.last_event()->phase == Phase{phase} &&
                    running.last_event()->tie_key == key && running.state() == RunState::running,
                    "Action context does not match admitted key");
            seen.push_back(tag);
        });
    };
    add(9,2,1,{});
    add(8,1,2,{});
    add(7,1,1,{std::numeric_limits<std::uint64_t>::max(),0,0,0});
    add(5,1,1,{2,0,0,0});
    add(4,1,1,{1,9,1,0});
    add(3,1,1,{1,9,0,1});
    add(1,1,1,{1,9,0,0});
    add(2,1,1,{1,9,0,0}); // Full equality must keep insertion order.
    add(0,0,8,{99,99,99,99});
    const auto result = loop.run();
    require(seen == std::vector<int>{0,1,2,3,4,5,7,8,9}, "time,phase,tie_key,insertion ordering");
    require(result == RunResult{SimTime{10},9} && loop.pending() == 0 && loop.executed() == 9 &&
            loop.state() == RunState::completed, "Completion state/count");
    require(loop.last_event()->at == SimTime{2}, "Final horizon jump fabricated an event");
}
void phases() {
    EventLoop loop{SimTime{6}};
    std::vector<int> trace;
    enqueue(loop,SimTime{5},Phase{5},{},[&](EventLoop&) { trace.push_back(5); });
    enqueue(loop,SimTime{5},Phase{4},{},[&](EventLoop&) { trace.push_back(40); });
    enqueue(loop,SimTime{5},Phase{4},{9,0,0,0},[&](EventLoop&) { trace.push_back(44); });
    enqueue(loop,SimTime{5},Phase{3},{},[&](EventLoop& current) {
        trace.push_back(3);
        const auto count = current.pending();
        rejects<std::invalid_argument>([&] { static_cast<void>(current.schedule(SimTime{4},Phase{99},{},[](EventLoop&) {})); },
                                       "Past event accepted");
        for (const auto phase : {1,2,3}) {
            rejects<std::invalid_argument>([&] {
                static_cast<void>(current.schedule(SimTime{5},Phase{phase},{999,999,999,999},[](EventLoop&) {}));
            }, "Same/earlier phase accepted at current time");
        }
        require(current.pending() == count, "Rejected insertion changed queue");
        const auto later = current.schedule_after(SimDuration{0},Phase{4},{},[&](EventLoop& next) {
            trace.push_back(4);
            require(next.now() == SimTime{5}, "Zero delay advanced time");
            static_cast<void>(next.schedule_after(SimDuration{0},Phase{8},{},[&](EventLoop&) { trace.push_back(8); }));
        });
        require(later && later->at == SimTime{5}, "Zero delay later phase rejected");
        enqueue(current,SimTime{6},Phase{1},{},[&](EventLoop&) { trace.push_back(61); });
    });
    static_cast<void>(loop.run());
    require(trace == std::vector<int>{3,40,4,44,5,8,61}, "Dynamic insertion or later-time phase reset");
    // An action stays alive even while its inserts reallocate the heap vector.
    EventLoop growth{SimTime{2}};
    std::size_t executed = 0;
    enqueue(growth,SimTime{0},Phase{1},{},[&](EventLoop& current) {
        for (std::uint64_t id = 0; id < 256; ++id) {
            enqueue(current,SimTime{1},Phase{1},{id,0,0,0},[&](EventLoop&) { ++executed; });
        }
        require(current.now() == SimTime{0}, "Heap growth changed current time");
    });
    require(growth.run().executed == 257 && executed == 256, "Heap growth invalidated executing action");
}
void horizon() {
    EventLoop empty{SimTime{15}};
    require(empty.run() == RunResult{SimTime{15},0} && !empty.last_event(), "Empty run must reach horizon");
    EventLoop zero{SimTime{0}};
    std::vector<int> phases_seen;
    for (int phase = 8; phase >= 1; --phase) {
        enqueue(zero,SimTime{0},Phase{phase},{},[&,phase](EventLoop&) { phases_seen.push_back(phase); });
    }
    require(!zero.schedule(SimTime{1},Phase{1},{},[](EventLoop&) {}), "Beyond-zero horizon admitted");
    require(zero.run().executed == 8 && phases_seen == std::vector<int>{1,2,3,4,5,6,7,8},
            "All phases on horizon must execute");
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    EventLoop large{SimTime{max}};
    int reached = 0;
    enqueue(large,SimTime{max-1},Phase{3},{},[&](EventLoop& current) {
        const auto pending = current.pending();
        require(!current.schedule_after(SimDuration{max},Phase{1},{},[](EventLoop&) {}),
                "Beyond-horizon delay overflowed or was admitted");
        require(current.pending() == pending, "Beyond horizon changed queue");
        const auto final = current.schedule_after(SimDuration{1},Phase{1},{},[&](EventLoop& at_end) {
            require(at_end.now() == SimTime{max}, "Inclusive int64 maximum horizon");
            ++reached;
            require(!at_end.schedule_after(SimDuration{1},Phase{1},{},[](EventLoop&) {}), "Past maximum admitted");
            enqueue(at_end,at_end.now(),Phase{8},{},[&](EventLoop&) { ++reached; });
        });
        require(final && final->insertion_sequence == 2, "Omitted event consumed insertion sequence");
    });
    require(large.run() == RunResult{SimTime{max},3} && reached == 2, "Near-maximum horizon scheduling");
    EventLoop inclusive{SimTime{10}};
    require(!inclusive.schedule(SimTime{11},Phase{1},{},[](EventLoop&) {}), "Outside horizon admitted");
    const auto admitted = inclusive.schedule(SimTime{10},Phase{1},{},[](EventLoop&) {});
    require(admitted && admitted->insertion_sequence == 1 && inclusive.run().executed == 1,
            "Absolute horizon/omission semantics");
}
void failures() {
    rejects<std::invalid_argument>([] { static_cast<void>(Phase{0}); }, "Zero phase accepted");
    rejects<std::invalid_argument>([] { static_cast<void>(Phase{-1}); }, "Negative phase wrapped");
    require(Phase{std::numeric_limits<std::uint64_t>::max()}.value() == std::numeric_limits<std::uint64_t>::max(),
            "Generic phase range truncated");
    EventLoop inputs{SimTime{10}};
    rejects<std::invalid_argument>([&] { static_cast<void>(inputs.schedule(SimTime{0},Phase{1},{},{})); }, "Empty action accepted");
    rejects<std::invalid_argument>([&] { static_cast<void>(inputs.schedule_after(SimDuration{11},Phase{1},{},{})); },
                                   "Empty action hidden by horizon omission");
    const auto valid = inputs.schedule(SimTime{0},Phase{1},{},[](EventLoop&) {});
    require(valid && valid->insertion_sequence == 1 && inputs.state() == RunState::ready, "Invalid input changed ready state");
    static_cast<void>(inputs.run());
    rejects<std::logic_error>([&] { static_cast<void>(inputs.run()); }, "Completed run restarted");
    rejects<std::logic_error>([&] { static_cast<void>(inputs.schedule(SimTime{10},Phase{1},{},[](EventLoop&) {})); },
                             "Completed loop admitted more work");
    EventLoop failed{SimTime{10}};
    bool later = false;
    enqueue(failed,SimTime{2},Phase{1},{},[](EventLoop& current) {
        enqueue(current,SimTime{3},Phase{1},{},[](EventLoop&) {});
        throw std::runtime_error("Action failure");
    });
    enqueue(failed,SimTime{4},Phase{1},{},[&](EventLoop&) { later = true; });
    rejects<std::runtime_error>([&] { static_cast<void>(failed.run()); }, "Action exception swallowed");
    require(failed.state() == RunState::failed && failed.now() == SimTime{2} && failed.last_event()->at == SimTime{2} &&
            failed.executed() == 0 && failed.pending() == 2 && !later, "Action failure reported normal completion or kept running");
    rejects<std::logic_error>([&] { static_cast<void>(failed.run()); }, "Failed loop restarted");
    rejects<std::logic_error>([&] { static_cast<void>(failed.schedule_after(SimDuration{1},Phase{1},{},[](EventLoop&) {})); },
                             "Failed loop admitted more work");
    EventLoop illegal{SimTime{10}};
    enqueue(illegal,SimTime{1},Phase{2},{},[](EventLoop& current) {
        enqueue(current,SimTime{1},Phase{2},{},[](EventLoop&) {});
    });
    rejects<std::invalid_argument>([&] { static_cast<void>(illegal.run()); }, "Unhandled illegal insertion did not abort run");
    require(illegal.state() == RunState::failed, "Illegal insertion failure not retained");
    EventLoop nested{SimTime{10}};
    enqueue(nested,SimTime{1},Phase{1},{},[](EventLoop& current) { static_cast<void>(current.run()); });
    rejects<std::logic_error>([&] { static_cast<void>(nested.run()); }, "Reentrant run accepted");
    require(nested.state() == RunState::failed, "Uncaught nested run must fail outer run");
}
void limits() {
    rejects<std::invalid_argument>([] { EventLoop invalid{SimTime{10},-1}; }, "Negative event limit wrapped");
    EventLoop zero_empty{SimTime{10},0};
    require(zero_empty.run().executed == 0, "Zero limit with empty queue is complete");
    EventLoop zero_busy{SimTime{10},0};
    bool ran = false;
    enqueue(zero_busy,SimTime{1},Phase{1},{},[&](EventLoop&) { ran = true; });
    rejects<EventLimitExceeded>([&] { static_cast<void>(zero_busy.run()); }, "Zero event limit ignored");
    require(!ran && zero_busy.pending() == 1 && zero_busy.now() == SimTime{0} && !zero_busy.last_event() &&
            zero_busy.state() == RunState::failed, "Limit popped event or advanced to horizon");
    EventLoop exact{SimTime{5},2};
    enqueue(exact,SimTime{0},Phase{1},{},[](EventLoop& current) {
        enqueue(current,SimTime{5},Phase{8},{},[](EventLoop&) {});
    });
    require(exact.run() == RunResult{SimTime{5},2}, "Exact limit with no remaining event must succeed");
    EventLoop boundary{SimTime{0},1};
    enqueue(boundary,SimTime{0},Phase{1},{},[](EventLoop&) {});
    enqueue(boundary,SimTime{0},Phase{2},{},[](EventLoop&) {});
    rejects<EventLimitExceeded>([&] { static_cast<void>(boundary.run()); }, "Limit on horizon reported completion");
    require(boundary.now() == boundary.horizon() && boundary.state() == RunState::failed && boundary.pending() == 1,
            "Clock at horizon must not imply successful completion");
    EventLoop recurring{SimTime{10},3};
    EventLoop::Action repeat;
    repeat = [&](EventLoop& current) {
        static_cast<void>(current.schedule_after(SimDuration{1},Phase{1},{},repeat));
    };
    enqueue(recurring,SimTime{0},Phase{1},{},repeat);
    rejects<EventLimitExceeded>([&] { static_cast<void>(recurring.run()); }, "Recurring actions evaded limit");
    require(recurring.executed() == 3 && recurring.now() == SimTime{2} && recurring.pending() == 1 &&
            recurring.state() == RunState::failed, "Event limit masqueraded as horizon completion");
}
std::vector<EventKey> trace(bool reverse_distinct) {
    EventLoop loop{SimTime{20}};
    std::vector<EventKey> observed;
    for (std::uint64_t i = 0; i < 128; ++i) {
        const auto label = reverse_distinct ? 127-i : i;
        enqueue(loop,SimTime{label%7},Phase{1+label%3},{label,0,0,0},[&](EventLoop& current) {
            observed.push_back(*current.last_event());
            require(current.now() == observed.back().at, "Clock differs from key");
        });
    }
    for (int i = 0; i < 16; ++i) {
        enqueue(loop,SimTime{8},Phase{2},{},[&](EventLoop& current) { observed.push_back(*current.last_event()); });
    }
    require(loop.run().executed == 144, "Repeat trace size");
    for (std::size_t i = 1; i < observed.size(); ++i) {
        require(observed[i-1] < observed[i], "Observed keys not strictly ordered");
    }
    return observed;
}
void determinism() {
    const auto first = trace(false);
    require(first == trace(false), "Repeated runs changed complete canonical keys");
    const auto permuted = trace(true);
    require(first.size() == permuted.size(), "Permutation lost events");
    for (std::size_t i = 0; i < first.size(); ++i) {
        require(first[i].at == permuted[i].at && first[i].phase == permuted[i].phase &&
                first[i].tie_key == permuted[i].tie_key, "Distinct keys depend on insertion order");
    }
    for (std::size_t i = 129; i < first.size(); ++i) {
        require(first[i].insertion_sequence == first[i-1].insertion_sequence+1, "Equal keys lost FIFO insertion order");
    }
}
}  // namespace
int main(int argc,char* argv[]) {
    try {
        if (argc != 2) { throw std::runtime_error("Expected group"); }
        const std::string_view group{argv[1]};
        if (group == "ordering") { ordering(); }
        else if (group == "phases") { phases(); }
        else if (group == "horizon") { horizon(); }
        else if (group == "failures") { failures(); }
        else if (group == "limits") { limits(); }
        else if (group == "determinism") { determinism(); }
        else { throw std::runtime_error("Unknown group"); }
        std::cout << group << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Engine test failed: " << e.what() << '\n';
        return 1;
    }
}
