#include "telemetry/common/ids.hpp"
#include "telemetry/common/time.hpp"

#include <cmath>
#include <concepts>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace telemetry;
constexpr auto max_ticks = std::numeric_limits<std::int64_t>::max();
constexpr auto max_id = std::numeric_limits<std::uint64_t>::max();

static_assert(!std::constructible_from<SimTime, double>);
static_assert(!std::constructible_from<SimDuration, long double>);
static_assert(!std::constructible_from<SimTime, bool>);
static_assert(!std::convertible_to<SimDuration, SimTime>);
static_assert(!std::convertible_to<SimTime, SimDuration>);
static_assert(!std::constructible_from<TaskId, NodeId>);
static_assert(!std::constructible_from<NodeId, double>);
static_assert(!std::constructible_from<NodeId, bool>);
static_assert(!std::default_initializable<NodeId>);
static_assert((SimTime{2} + SimDuration{3}).ticks() == 5);

void require(bool condition, std::string_view message) {
    if (!condition) { throw std::runtime_error(std::string{message}); }
}
template<class Exception, class Action>
void rejects(Action action, std::string_view message) {
    try { action(); }
    catch (const Exception&) { return; }
    throw std::runtime_error(std::string{message});
}

void time_checks() {
    require(SimTime{}.ticks() == 0 && SimDuration{}.ticks() == 0, "Zero is a real time");
    require(SimTime{max_ticks}.ticks() == max_ticks, "Max integer time must remain exact");
    require(SimTime{static_cast<std::uint64_t>(max_ticks)}.ticks() == max_ticks, "Unsigned maximum valid time");
    require(SimTime{UINT64_C(9007199254740993)}.ticks() == INT64_C(9007199254740993),
            "Integer time must not pass through floating point");
    rejects<std::invalid_argument>([] { static_cast<void>(SimTime{-1}); }, "Negative time accepted");
    rejects<std::invalid_argument>([] { static_cast<void>(SimDuration{-1}); }, "Negative duration accepted");
    rejects<std::overflow_error>([] { static_cast<void>(SimTime{max_id}); }, "Unsigned time overflow accepted");
    rejects<std::overflow_error>([] { static_cast<void>(SimTime{UINT64_C(1) << 63}); }, "First unsigned out-of-range time");
    rejects<std::invalid_argument>([] { static_cast<void>(SimTime{std::numeric_limits<std::int64_t>::min()}); }, "Minimum signed time accepted");
    rejects<std::overflow_error>([] { static_cast<void>(SimDuration{max_id}); }, "Unsigned duration overflow accepted");
    require(SimTime{3} + SimDuration{4} == SimTime{7}, "Time addition");
    require(SimTime{7} - SimDuration{4} == SimTime{3}, "Time subtraction");
    require(SimTime{7} - SimTime{3} == SimDuration{4}, "Elapsed time");
    require(SimDuration{4} + SimDuration{3} == SimDuration{7}, "Duration addition");
    require(SimDuration{4} - SimDuration{3} == SimDuration{1}, "Duration subtraction");
    require(SimTime{max_ticks} + SimDuration{0} == SimTime{max_ticks}, "Max plus zero");
    require(SimTime{0} + SimDuration{max_ticks} == SimTime{max_ticks}, "Exact boundary sum");
    rejects<std::overflow_error>([] { static_cast<void>(SimTime{max_ticks} + SimDuration{1}); }, "Time addition overflow");
    rejects<std::overflow_error>([] { static_cast<void>(SimDuration{max_ticks} + SimDuration{1}); }, "Duration addition overflow");
    rejects<std::invalid_argument>([] { static_cast<void>(SimTime{0} - SimTime{1}); }, "Negative elapsed accepted");
    rejects<std::invalid_argument>([] { static_cast<void>(SimTime{0} - SimDuration{1}); }, "Negative time subtraction");
    rejects<std::invalid_argument>([] { static_cast<void>(SimDuration{0} - SimDuration{1}); }, "Negative duration subtraction");
    require(deadline_within(SimTime{max_ticks-1}, SimDuration{1}, SimTime{max_ticks}) == SimTime{max_ticks},
            "Horizon must be inclusive");
    require(!deadline_within(SimTime{max_ticks-1}, SimDuration{max_ticks}, SimTime{max_ticks}),
            "Beyond horizon must not overflow");
    require(deadline_within(SimTime{max_ticks}, SimDuration{0}, SimTime{max_ticks}) == SimTime{max_ticks},
            "Zero delay at horizon");
    rejects<std::invalid_argument>([] { static_cast<void>(deadline_within(SimTime{2}, SimDuration{0}, SimTime{1})); },
                                   "Time beyond horizon accepted");
    const std::optional<SimTime> absent;
    const std::optional<SimTime> at_zero{SimTime{0}};
    require(!absent && at_zero && at_zero->ticks() == 0, "Absence differs from zero");
    require(seconds(SimDuration{ticks_per_second}) == 1.0L && tick_ns == 1000, "Time scale");
}

void numeric_checks() {
    const auto nan = std::numeric_limits<long double>::quiet_NaN();
    const auto infinity = std::numeric_limits<long double>::infinity();
    require(ceil_ticks(0).ticks() == 0 && ceil_ticks(2).ticks() == 2, "Exact ceiling");
    require(ceil_ticks(2.25L).ticks() == 3, "Fraction must round upward");
    require(ceil_ticks(std::nextafter(2.0L, infinity)).ticks() == 3, "No rounding epsilon");
    require(ceil_ticks(std::ldexp(1.0L, 62)).ticks() == INT64_C(4611686018427387904), "Large exact ceiling");
    const auto upper = std::ldexp(1.0L, 63);
    rejects<std::overflow_error>([&] { static_cast<void>(ceil_ticks(upper)); }, "2^63 float cast must be rejected");
    require(ceil_ticks(std::floor(std::nextafter(upper, 0.0L))).ticks() > 0, "Largest representable safe value");
    if constexpr (std::numeric_limits<long double>::digits >= 64) {
        require(ceil_ticks(static_cast<long double>(max_ticks)).ticks() == max_ticks, "Exact long double INT64_MAX");
        rejects<std::overflow_error>([] { static_cast<void>(round_thresholds(static_cast<long double>(max_ticks),
                                                       static_cast<long double>(max_ticks))); }, "Suspect plus one overflow");
    }
    for (auto bad : {-1.0L, nan, infinity}) {
        rejects<std::invalid_argument>([&] { static_cast<void>(ceil_ticks(bad)); }, "Invalid duration accepted");
    }
    require(processing_duration(4, 1'000'000).ticks() == 4, "Processing units");
    require(processing_duration(1, 2'000'000).ticks() == 1, "Positive sub-tick processing");
    require(processing_duration(3, 2'000'000).ticks() == 2, "Fractional processing ceiling");
    for (auto bad : {0.0L, -1.0L, nan, infinity}) {
        rejects<std::invalid_argument>([&] { static_cast<void>(processing_duration(bad, 1)); }, "Invalid work accepted");
        rejects<std::invalid_argument>([&] { static_cast<void>(processing_duration(1, bad)); }, "Invalid performance accepted");
    }
    const auto largest = std::numeric_limits<long double>::max();
    const auto smallest = std::numeric_limits<long double>::min();
    rejects<std::overflow_error>([&] { static_cast<void>(processing_duration(largest, smallest)); }, "Processing infinity");
    rejects<std::overflow_error>([&] { static_cast<void>(processing_duration(largest, 1)); }, "Scaled ticks infinity");
    rejects<std::underflow_error>([&] { static_cast<void>(processing_duration(smallest, largest)); }, "Silent processing underflow");
    rejects<std::overflow_error>([] { static_cast<void>(processing_duration(1e13L, 1)); }, "Finite out-of-range ticks");
    const auto thresholds = round_thresholds(10.1L, 20.2L);
    require(thresholds.suspect.ticks() == 11 && thresholds.failed.ticks() == 21,
            "Second threshold must use the raw first threshold");
    require(thresholds.raw_suspect_ticks == 10.1L && thresholds.raw_failed_ticks == 20.2L, "Raw diagnostics lost");
    const auto close = round_thresholds(10.1L, 10.11L);
    require(close.suspect.ticks() == 11 && close.failed.ticks() == 12, "Close thresholds must keep SUSPECT phase");
    const auto tiny = round_thresholds(0.01L, 0.011L);
    require(tiny.suspect.ticks() == 1 && tiny.failed.ticks() == 2, "Sub-tick thresholds");
    const auto equal = round_thresholds(10, 10);
    require(equal.suspect.ticks() == 10 && equal.failed.ticks() == 11, "Represented raw thresholds may coincide");
    for (auto bad : {0.0L, -1.0L, nan, infinity}) {
        rejects<std::invalid_argument>([&] { static_cast<void>(round_thresholds(bad, 20)); }, "Invalid suspect threshold");
    }
    rejects<std::invalid_argument>([] { static_cast<void>(round_thresholds(10, 9)); }, "Reversed thresholds accepted");
    rejects<std::invalid_argument>([&] { static_cast<void>(round_thresholds(10, nan)); }, "NaN failed threshold");
    rejects<std::invalid_argument>([&] { static_cast<void>(round_thresholds(10, infinity)); }, "Infinite failed threshold");
    rejects<std::overflow_error>([&] { static_cast<void>(round_thresholds(1, upper)); }, "Failed threshold range");
}

void id_checks() {
    require(NodeId{1}.value() == 1 && TaskId{2}.value() == 2, "ID representation");
    require(HeartbeatSeq{1}.next() == HeartbeatSeq{2}, "Heartbeat increment");
    require(AttemptNo{1}.next() == AttemptNo{2}, "Attempt increment");
    require(NodeId{max_id}.value() == max_id, "Full unsigned ID range");
    require(NodeId{1} < NodeId{2}, "Stable ID order");
    rejects<std::invalid_argument>([] { static_cast<void>(NodeId{0}); }, "Zero ID accepted");
    rejects<std::invalid_argument>([] { static_cast<void>(TaskId{-1}); }, "Negative ID became unsigned");
    rejects<std::invalid_argument>([] { static_cast<void>(HeartbeatSeq{0}); }, "Zero heartbeat accepted");
    rejects<std::invalid_argument>([] { static_cast<void>(AttemptNo{0}); }, "Zero attempt accepted");
    rejects<std::overflow_error>([] { static_cast<void>(AttemptNo{max_id}.next()); }, "Attempt overflow");
    rejects<std::overflow_error>([] { static_cast<void>(HeartbeatSeq{max_id}.next()); }, "Sequence overflow");
    require(checked_increment(0) == 1 && checked_increment(max_id-1) == max_id, "Generation/counter boundaries");
    rejects<std::overflow_error>([] { static_cast<void>(checked_increment(max_id)); }, "Counter overflow");
    rejects<std::invalid_argument>([] { static_cast<void>(checked_increment(-2)); }, "Negative counter accepted");
}
}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) { throw std::runtime_error("Expected test group"); }
        const std::string_view group{argv[1]};
        if (group == "time") { time_checks(); }
        else if (group == "numeric") { numeric_checks(); }
        else if (group == "ids") { id_checks(); }
        else { throw std::runtime_error("Unknown test group"); }
        std::cout << group << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Common type test failed: " << error.what() << '\n';
        return 1;
    }
}
