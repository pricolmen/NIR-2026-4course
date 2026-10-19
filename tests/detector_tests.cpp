#include "telemetry/core/detector.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace telemetry;
using namespace telemetry::core;
constexpr SimTime horizon{10'000};
const TimeoutParameters parameters{SimDuration{10}, SimDuration{20}, 2};
void require(bool ok, std::string_view message) {
    if (!ok) { throw std::runtime_error(std::string{message}); }
}
template<class Exception, class Action>
void rejects(Action action, std::string_view message) {
    try { action(); }
    catch (const Exception&) { return; }
    throw std::runtime_error(std::string{message});
}
HeartbeatUpdate heartbeat(Detector& d, std::int64_t at, std::uint64_t seq, SimTime end = horizon) {
    return d.receive_heartbeat(SimTime{at}, {d.node(), HeartbeatSeq{seq}}, end);
}
void fire(Detector& d, const TimeoutRequest& request) {
    static_cast<void>(d.receive_timeout(request.at, request));
}
void thresholds(const Detector& d, std::int64_t suspect, std::int64_t failed) {
    require(d.thresholds().durations.suspect == SimDuration{suspect}, "Wrong suspect threshold");
    require(d.thresholds().durations.failed == SimDuration{failed}, "Wrong failed threshold");
}
void fixed() {
    Detector d{NodeId{7}, parameters, FixedTimeout{}};
    require(d.status() == NodeStatus::alive && d.last_contact() == SimTime{} &&
            d.last_sequence() == 0 && d.generation() == 0 && d.intervals().empty(), "Initial observation");
    const auto initial = d.timer_requests(horizon);
    require(initial.size() == 2 && initial[0] == TimeoutRequest{NodeId{7},SimTime{20},TimeoutKind::suspect,0} &&
            initial[1] == TimeoutRequest{NodeId{7},SimTime{40},TimeoutKind::failed,0}, "Both initial deadlines");
    fire(d, initial[0]);
    require(d.status() == NodeStatus::suspect, "Missing first heartbeat must cause SUSPECT");
    fire(d, initial[1]);
    require(d.status() == NodeStatus::failed, "Missing first heartbeat must cause FAILED from contact zero");
    require(d.timer_requests(horizon).empty(), "Terminal state requests no timers");
    const auto ignored = heartbeat(d, 41, 1);
    require(ignored.decision == HeartbeatDecision::already_failed && ignored.timers.empty() &&
            d.last_sequence() == 0 && d.last_contact() == SimTime{} && d.generation() == 0,
            "FAILED must be final, including sequence/contact");
    Detector resumed{NodeId{1}, parameters, FixedTimeout{}};
    auto update = heartbeat(resumed, 10, 1);
    require(update.timers[0].at == SimTime{30} && update.timers[1].at == SimTime{50}, "Contact-based timers");
    fire(resumed, update.timers[0]);
    update = heartbeat(resumed, 31, 2);
    require(update.before == NodeStatus::suspect && update.after == NodeStatus::alive, "SUSPECT recovery");
    require(resumed.generation() == 2 && update.timers[1].at == SimTime{71}, "Recovery replaces both deadlines");
    fire(resumed, update.timers[1]);
    require(resumed.status() == NodeStatus::failed, "Failure check first, even if suspect timer was not delivered");
    thresholds(resumed, 20, 40);
    require(resumed.intervals().empty(), "Fixed policy does not retain an unused window");
}
void adaptive() {
    Detector d{NodeId{1}, parameters, AdaptiveTimeout{3,3,2}};
    auto update = heartbeat(d, 100, 1);
    require(d.intervals().empty() && !d.thresholds().statistics.mean_ticks && !d.thresholds().warmed_up,
            "Artificial zero contact is not an interval");
    thresholds(d, 20, 40);
    update = heartbeat(d, 110, 2);
    require(d.intervals().size() == 1 && d.thresholds().statistics.mean_ticks == 10 &&
            !d.thresholds().statistics.sample_std_ticks && !d.thresholds().warmed_up, "One interval warmup");
    update = heartbeat(d, 122, 3);
    thresholds(d, 20, 40);
    require(!d.thresholds().warmed_up, "m_min needs m_min+1 messages");
    update = heartbeat(d, 130, 4);
    const auto stats = d.thresholds().statistics;
    require(stats.count == 3 && stats.mean_ticks == 10 && stats.sample_std_ticks == 2 &&
            d.thresholds().warmed_up, "Sample deviation for [10,12,8] is 2, not population deviation");
    thresholds(d, 14, 28);
    require(update.timers[0].at == SimTime{144} && update.timers[1].at == SimTime{158}, "Adaptive deadlines");
    const auto raw = d.thresholds().durations.raw_suspect_ticks;
    fire(d, update.timers[0]);
    thresholds(d, 14, 28);
    require(d.thresholds().durations.raw_suspect_ticks == raw && d.intervals().size() == 3,
            "No messages must freeze thresholds/history in SUSPECT");
    fire(d, update.timers[1]);
    require(d.status() == NodeStatus::failed, "Failure deadline is contact+28, not suspect+28");
    const auto late = heartbeat(d, 159, 5);
    require(late.decision == HeartbeatDecision::already_failed && d.last_sequence() == 4 &&
            d.thresholds().statistics.sample_std_ticks == 2, "Late heartbeat cannot update adaptive history");
    Detector initial{NodeId{2}, parameters, AdaptiveTimeout{3,2,0}};
    const auto timers = initial.timer_requests(horizon);
    fire(initial, timers[0]); fire(initial, timers[1]);
    require(initial.status() == NodeStatus::failed, "Adaptive initial timers with no first message");
}
void window() {
    Detector d{NodeId{1}, parameters, AdaptiveTimeout{3,2,2}};
    static_cast<void>(heartbeat(d, 1, 1));
    static_cast<void>(heartbeat(d, 11, 2));
    static_cast<void>(heartbeat(d, 23, 3));
    static_cast<void>(heartbeat(d, 31, 4));
    static_cast<void>(heartbeat(d, 31, 5));
    auto intervals = d.intervals();
    require(intervals.size() == 3 && intervals[0] == SimDuration{12} &&
            intervals[1] == SimDuration{8} && intervals[2] == SimDuration{0}, "Evict oldest; retain zero interval");
    static_cast<void>(heartbeat(d, 101, 12));
    intervals = d.intervals();
    require(intervals.size() == 3 && intervals[0] == SimDuration{8} &&
            intervals[1] == SimDuration{0} && intervals[2] == SimDuration{70}, "Loss/sequence gaps retain large interval");
    Detector zeros{NodeId{2}, parameters, AdaptiveTimeout{2,2,100}};
    static_cast<void>(heartbeat(zeros, 0, 1));
    static_cast<void>(heartbeat(zeros, 0, 2));
    static_cast<void>(heartbeat(zeros, 0, 3));
    require(zeros.thresholds().warmed_up && zeros.thresholds().statistics.mean_ticks == 0 &&
            zeros.thresholds().statistics.sample_std_ticks == 0, "Zero intervals contribute to warmup");
    thresholds(zeros, 10, 20);
    Detector zero_coefficient{NodeId{3}, parameters, AdaptiveTimeout{2,2,0}};
    static_cast<void>(heartbeat(zero_coefficient, 0, 1));
    static_cast<void>(heartbeat(zero_coefficient, 10, 2));
    static_cast<void>(heartbeat(zero_coefficient, 40, 3));
    thresholds(zero_coefficient, 20, 40);
    require(zero_coefficient.thresholds().statistics.sample_std_ticks.has_value(), "c=0 still computes diagnostics");
}
void generations() {
    for (const TimeoutPolicy policy : {TimeoutPolicy{FixedTimeout{}},TimeoutPolicy{AdaptiveTimeout{3,2,2}}}) {
        Detector d{NodeId{1},parameters,policy};
        const auto original = d.timer_requests(horizon);
        auto first = heartbeat(d, 20, 1); // phase 4, before phase 5 at same time
        auto same = heartbeat(d, 20, 2);
        require(d.generation() == 2 && d.last_sequence() == 2, "Equal-time accepted message increments generation");
        const auto stale = d.receive_timeout(SimTime{20}, original[0]);
        require(stale.decision == TimeoutDecision::stale_generation && d.status() == NodeStatus::alive,
                "Heartbeat at suspect deadline invalidates old timer");
        const auto duplicate = heartbeat(d, 21, 2);
        const auto reordered = heartbeat(d, 22, 1);
        require(duplicate.decision == HeartbeatDecision::non_increasing_sequence &&
                reordered.decision == HeartbeatDecision::non_increasing_sequence && duplicate.timers.empty() &&
                d.generation() == 2 && d.last_contact() == SimTime{20}, "Invalid sequence cannot reset contact/timers");
        if (std::holds_alternative<AdaptiveTimeout>(policy)) {
            require(d.intervals().size() == 1 && d.intervals()[0] == SimDuration{0},
                    "Invalid sequence must not add intervals");
        }
        require(d.receive_timeout(SimTime{40}, first.timers[0]).decision == TimeoutDecision::stale_generation,
                "Same-time contact still invalidates the previous generation");
        fire(d, original[1]); // stale at 40
        fire(d, same.timers[0]);
        require(d.status() == NodeStatus::suspect, "Invalid messages do not prevent timeout");
        auto recovered = heartbeat(d, 60, 3); // equal to failed deadline, delivery wins
        require(recovered.after == NodeStatus::alive, "Heartbeat at failed deadline processed before timeout");
        require(d.receive_timeout(SimTime{60}, same.timers[1]).decision == TimeoutDecision::stale_generation,
                "Failed deadline invalidated by equal-time heartbeat");
        fire(d, recovered.timers[1]);
        require(d.status() == NodeStatus::failed, "Current timer still works after stale events");
        const auto terminal = d.receive_timeout(recovered.timers[1].at, recovered.timers[1]);
        require(terminal.decision == TimeoutDecision::already_failed, "Repeated failure timer is idempotent");
    }
}
void boundaries() {
    Detector d{NodeId{1}, {SimDuration{1},SimDuration{10},1.001L},FixedTimeout{}};
    thresholds(d, 10, 11);
    require(d.timer_requests(SimTime{9}).empty(), "Beyond horizon omitted");
    require(d.timer_requests(SimTime{10}).size() == 1, "Suspect at horizon included");
    require(d.timer_requests(SimTime{11}).size() == 2, "Failed at horizon included");
    Detector fractional{NodeId{2}, {SimDuration{1},SimDuration{1},1.01L},AdaptiveTimeout{2,2,0}};
    static_cast<void>(heartbeat(fractional, 0, 1));
    static_cast<void>(heartbeat(fractional, 1, 2));
    const auto result = heartbeat(fractional, 3, 3);
    thresholds(fractional, 2, 3);
    require(fractional.thresholds().durations.raw_suspect_ticks == 1.5L &&
            fractional.thresholds().durations.raw_failed_ticks == 1.01L*1.5L,
            "Raw failed uses raw suspect, then distinct rounded deadlines");
    require(result.timers[0].at == SimTime{5} && result.timers[1].at == SimTime{6}, "Fractional deadlines");
    const auto max = std::numeric_limits<std::int64_t>::max();
    Detector large{NodeId{3},parameters,FixedTimeout{}};
    const auto near_end = heartbeat(large,max-1,1,SimTime{max});
    require(near_end.timers.empty() && large.last_contact() == SimTime{max-1}, "Beyond int64 horizon no addition overflow");
    const auto end = heartbeat(large,max,2,SimTime{max});
    require(end.timers.empty() && large.generation() == 2, "Heartbeat at horizon allowed");
    Detector sequences{NodeId{4},parameters,FixedTimeout{}};
    static_cast<void>(heartbeat(sequences,0,std::numeric_limits<std::uint64_t>::max()));
    require(heartbeat(sequences,1,1).decision == HeartbeatDecision::non_increasing_sequence,
            "Unsigned max sequence preserved, never incremented on receipt");
}
void inputs() {
    const auto nan = std::numeric_limits<long double>::quiet_NaN();
    const auto inf = std::numeric_limits<long double>::infinity();
    for (const auto p : {TimeoutParameters{SimDuration{0},SimDuration{20},2},
                         TimeoutParameters{SimDuration{10},SimDuration{9},2},
                         TimeoutParameters{SimDuration{10},SimDuration{20},1},
                         TimeoutParameters{SimDuration{10},SimDuration{20},nan},
                         TimeoutParameters{SimDuration{10},SimDuration{20},inf}}) {
        rejects<std::invalid_argument>([&] { Detector d{NodeId{1},p,FixedTimeout{}}; }, "Invalid parameters accepted");
    }
    for (const auto policy : {AdaptiveTimeout{2,1,0},AdaptiveTimeout{1,2,0},AdaptiveTimeout{2,2,-1},
                              AdaptiveTimeout{2,2,nan},AdaptiveTimeout{2,2,inf}}) {
        rejects<std::invalid_argument>([&] { Detector d{NodeId{1},parameters,policy}; }, "Invalid adaptive parameters");
    }
    rejects<std::overflow_error>([] {
        Detector d{NodeId{1},{SimDuration{1},SimDuration{INT64_C(4611686018427387904)},2},FixedTimeout{}};
    }, "Out-of-range failed threshold");
    Detector d{NodeId{1},parameters,AdaptiveTimeout{2,2,2}};
    auto update = heartbeat(d,10,1);
    rejects<std::invalid_argument>([&] { static_cast<void>(heartbeat(d,9,2)); }, "Backward heartbeat accepted");
    rejects<std::invalid_argument>([&] { static_cast<void>(heartbeat(d,11,2,SimTime{10})); }, "Beyond horizon accepted");
    rejects<std::invalid_argument>([&] {
        static_cast<void>(d.receive_heartbeat(SimTime{11},{NodeId{2},HeartbeatSeq{2}},horizon));
    }, "Wrong node heartbeat accepted");
    rejects<std::invalid_argument>([&] { static_cast<void>(d.timer_requests(SimTime{9})); }, "Past horizon accepted");
    auto wrong = update.timers[0]; wrong.node = NodeId{2};
    rejects<std::invalid_argument>([&] { fire(d,wrong); }, "Wrong node timer accepted");
    wrong = update.timers[0]; wrong.at = SimTime{29};
    rejects<std::invalid_argument>([&] { fire(d,wrong); }, "Early forged timer accepted");
    wrong = update.timers[0]; wrong.generation = 99;
    require(d.receive_timeout(wrong.at,wrong).decision == TimeoutDecision::stale_generation,
            "Unmatched generation is safely ignored");
    require(d.generation() == 1 && d.last_contact() == SimTime{10} && d.intervals().empty(), "Invalid inputs preserve detector state");
    fire(d,update.timers[0]);
    rejects<std::invalid_argument>([&] { static_cast<void>(heartbeat(d,29,2)); }, "Ignored event still advances observation time");
    // Finite but enormous c overflows adaptive arithmetic only after warmup.
    Detector overflow{NodeId{2},parameters,AdaptiveTimeout{2,2,std::numeric_limits<long double>::max()}};
    static_cast<void>(heartbeat(overflow,0,1));
    static_cast<void>(heartbeat(overflow,1,2));
    rejects<std::overflow_error>([&] { static_cast<void>(heartbeat(overflow,101,3)); }, "Adaptive arithmetic overflow");
    require(overflow.generation() == 2 && overflow.last_sequence() == 2 && overflow.intervals().size() == 1 &&
            overflow.last_contact() == SimTime{1}, "Numerical error must not partially commit heartbeat");
    update = heartbeat(overflow,2,3);
    thresholds(overflow,10,20);
    require(update.decision == HeartbeatDecision::accepted, "Failed update must not advance observation clock");
    if (std::numeric_limits<long double>::has_denorm == std::denorm_present) {
        Detector tiny{NodeId{3},parameters,AdaptiveTimeout{4,4,std::numeric_limits<long double>::denorm_min()}};
        for (std::uint64_t seq = 1; seq <= 4; ++seq) { static_cast<void>(heartbeat(tiny,0,seq)); }
        rejects<std::underflow_error>([&] { static_cast<void>(heartbeat(tiny,1,5)); }, "Silent margin underflow");
        require(tiny.generation() == 4 && tiny.intervals().size() == 3, "Underflow does not commit");
    }
}
}  // namespace
int main(int argc, char* argv[]) {
    try {
        if (argc != 2) { throw std::runtime_error("Expected group"); }
        const std::string_view group{argv[1]};
        if (group == "fixed") { fixed(); }
        else if (group == "adaptive") { adaptive(); }
        else if (group == "window") { window(); }
        else if (group == "generations") { generations(); }
        else if (group == "boundaries") { boundaries(); }
        else if (group == "inputs") { inputs(); }
        else { throw std::runtime_error("Unknown group"); }
        std::cout << group << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Detector test failed: " << e.what() << '\n';
        return 1;
    }
}
