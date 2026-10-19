#include "telemetry/core/detector.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace telemetry::core {
namespace {
DetectorThresholds calculate(const TimeoutParameters& p, FixedTimeout,
                             std::span<const SimDuration>) {
    const auto suspect = static_cast<long double>(p.initial_timeout.ticks());
    return {round_thresholds(suspect, p.kappa * suspect), {}, false};
}
DetectorThresholds calculate(const TimeoutParameters& p, const AdaptiveTimeout& policy,
                             std::span<const SimDuration> intervals) {
    auto result = calculate(p, FixedTimeout{}, {});
    auto& stats = result.statistics;
    stats.count = intervals.size();
    if (intervals.empty()) { return result; }
    // Two passes over the bounded window. All intervals are nonnegative and
    // int64-bounded; long double has ample exponent range for these sums/squares.
    long double sum = 0;
    for (const auto interval : intervals) { sum += static_cast<long double>(interval.ticks()); }
    if (!std::isfinite(sum)) { throw std::overflow_error("Interval sum overflow"); }
    const auto mean = sum / static_cast<long double>(intervals.size());
    if (!std::isfinite(mean)) { throw std::overflow_error("Interval mean overflow"); }
    stats.mean_ticks = mean;
    if (intervals.size() < 2) { return result; }
    long double squared = 0;
    for (const auto interval : intervals) {
        const auto deviation = static_cast<long double>(interval.ticks()) - mean;
        squared += deviation * deviation;
    }
    if (!std::isfinite(squared)) { throw std::overflow_error("Interval variance overflow"); }
    const auto stddev = std::sqrt(squared / static_cast<long double>(intervals.size() - 1));
    if (!std::isfinite(stddev)) { throw std::overflow_error("Interval deviation overflow"); }
    stats.sample_std_ticks = stddev;
    if (intervals.size() < policy.min_intervals) { return result; }
    const auto margin = policy.coefficient * stddev;
    if (policy.coefficient > 0 && stddev > 0 && margin == 0) {
        throw std::underflow_error("Adaptive margin lost representability");
    }
    const auto estimate = mean + margin;
    if (!std::isfinite(margin) || !std::isfinite(estimate)) {
        throw std::overflow_error("Adaptive threshold overflow");
    }
    const auto suspect = std::max(static_cast<long double>(p.heartbeat_period.ticks()), estimate);
    result.durations = round_thresholds(suspect, p.kappa * suspect);
    result.warmed_up = true;
    return result;
}
DetectorThresholds calculate(const TimeoutParameters& p, const TimeoutPolicy& policy,
                             std::span<const SimDuration> intervals) {
    return std::visit([&](const auto& selected) { return calculate(p, selected, intervals); }, policy);
}
std::vector<TimeoutRequest> requests(NodeId node, SimTime contact, std::uint64_t generation,
                                     const RoundedThresholds& thresholds, SimTime horizon) {
    std::vector<TimeoutRequest> result;
    result.reserve(2);
    if (const auto at = deadline_within(contact, thresholds.suspect, horizon)) {
        result.push_back({node, *at, TimeoutKind::suspect, generation});
    }
    if (const auto at = deadline_within(contact, thresholds.failed, horizon)) {
        result.push_back({node, *at, TimeoutKind::failed, generation});
    }
    return result;
}
}  // namespace

Detector::Detector(NodeId node, TimeoutParameters parameters, TimeoutPolicy policy)
    : node_(node), parameters_(parameters), policy_(policy) {
    if (parameters_.heartbeat_period.ticks() == 0 ||
        parameters_.initial_timeout < parameters_.heartbeat_period ||
        !std::isfinite(parameters_.kappa) || parameters_.kappa <= 1) {
        throw std::invalid_argument("Require H > 0, tau0 >= H and finite kappa > 1");
    }
    if (const auto* adaptive = std::get_if<AdaptiveTimeout>(&policy_)) {
        if (adaptive->min_intervals < 2 || adaptive->window_size < adaptive->min_intervals ||
            !std::isfinite(adaptive->coefficient) || adaptive->coefficient < 0) {
            throw std::invalid_argument("Require M >= m_min >= 2 and finite c >= 0");
        }
    }
    thresholds_ = calculate(parameters_, policy_, {});
}
void Detector::check_time(SimTime at) const {
    if (at < last_observation_) { throw std::invalid_argument("Detector observations must be monotonic"); }
}
std::vector<TimeoutRequest> Detector::timer_requests(SimTime horizon) const {
    if (horizon < last_observation_) { throw std::invalid_argument("Horizon precedes observation"); }
    if (status_ == NodeStatus::failed) { return {}; }
    return requests(node_, last_contact_, generation_, thresholds_.durations, horizon);
}
HeartbeatUpdate Detector::receive_heartbeat(SimTime at, const HeartbeatMessage& message, SimTime horizon) {
    check_time(at);
    if (message.node != node_) { throw std::invalid_argument("Heartbeat belongs to another node"); }
    if (at > horizon) { throw std::invalid_argument("Heartbeat exceeds horizon"); }
    const auto before = status_;
    if (status_ == NodeStatus::failed || message.sequence.value() <= last_sequence_) {
        last_observation_ = at;
        return {status_ == NodeStatus::failed ? HeartbeatDecision::already_failed :
                HeartbeatDecision::non_increasing_sequence, before, status_, {}};
    }
    const auto next_generation = checked_increment(generation_);
    auto next_intervals = intervals_;
    if (const auto* adaptive = std::get_if<AdaptiveTimeout>(&policy_); adaptive && last_real_contact_) {
        if (next_intervals.size() == adaptive->window_size) { next_intervals.erase(next_intervals.begin()); }
        next_intervals.push_back(at - *last_real_contact_);
    }
    const auto next_thresholds = calculate(parameters_, policy_, next_intervals);
    auto timers = requests(node_, at, next_generation, next_thresholds.durations, horizon);
    // All allocations and arithmetic precede the logical commit.
    intervals_.swap(next_intervals);
    thresholds_ = next_thresholds;
    last_contact_ = at;
    last_real_contact_ = at;
    last_sequence_ = message.sequence.value();
    generation_ = next_generation;
    status_ = NodeStatus::alive;
    last_observation_ = at;
    return {HeartbeatDecision::accepted, before, status_, std::move(timers)};
}
TimeoutUpdate Detector::receive_timeout(SimTime at, const TimeoutRequest& request) {
    check_time(at);
    if (request.node != node_ || request.at != at ||
        (request.kind != TimeoutKind::suspect && request.kind != TimeoutKind::failed)) {
        throw std::invalid_argument("Invalid timeout request");
    }
    const auto before = status_;
    if (request.generation != generation_ || status_ == NodeStatus::failed) {
        last_observation_ = at;
        return {request.generation != generation_ ? TimeoutDecision::stale_generation :
                TimeoutDecision::already_failed, before, status_};
    }
    const auto elapsed = at - last_contact_;
    const auto expected = request.kind == TimeoutKind::suspect ? thresholds_.durations.suspect :
                                                                thresholds_.durations.failed;
    if (elapsed != expected) { throw std::invalid_argument("Timeout does not match current deadline"); }
    // Both deadlines are measured from contact, never from entry into SUSPECT.
    if (elapsed >= thresholds_.durations.failed) { status_ = NodeStatus::failed; }
    else if (elapsed >= thresholds_.durations.suspect) { status_ = NodeStatus::suspect; }
    last_observation_ = at;
    return {TimeoutDecision::applied, before, status_};
}
}  // namespace telemetry::core
