#pragma once

#include "telemetry/common/messages.hpp"
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace telemetry::core {
enum class NodeStatus { alive, suspect, failed };
enum class TimeoutKind { suspect, failed };
enum class HeartbeatDecision { accepted, non_increasing_sequence, already_failed };
enum class TimeoutDecision { applied, stale_generation, already_failed };

struct TimeoutParameters {
    SimDuration heartbeat_period;
    SimDuration initial_timeout;
    long double kappa;
};
struct FixedTimeout {};
struct AdaptiveTimeout {
    std::size_t window_size;
    std::size_t min_intervals;
    long double coefficient;
};
using TimeoutPolicy = std::variant<FixedTimeout, AdaptiveTimeout>;

struct IntervalStatistics {
    std::size_t count = 0;
    std::optional<long double> mean_ticks;
    std::optional<long double> sample_std_ticks;
};
struct DetectorThresholds {
    RoundedThresholds durations;
    IntervalStatistics statistics;
    bool warmed_up = false;
};
struct TimeoutRequest {
    NodeId node;
    SimTime at;
    TimeoutKind kind;
    std::uint64_t generation;
    bool operator==(const TimeoutRequest&) const = default;
};
struct HeartbeatUpdate {
    HeartbeatDecision decision;
    NodeStatus before;
    NodeStatus after;
    std::vector<TimeoutRequest> timers;
};
struct TimeoutUpdate {
    TimeoutDecision decision;
    NodeStatus before;
    NodeStatus after;
};

// One detector per node. Inputs contain observations only, never physical state.
// The adapter must process heartbeat deliveries before timeouts at equal times.
class Detector {
public:
    Detector(NodeId node, TimeoutParameters parameters, TimeoutPolicy policy);
    [[nodiscard]] NodeId node() const noexcept { return node_; }
    [[nodiscard]] NodeStatus status() const noexcept { return status_; }
    [[nodiscard]] SimTime last_contact() const noexcept { return last_contact_; }
    [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const DetectorThresholds& thresholds() const noexcept { return thresholds_; }
    [[nodiscard]] std::span<const SimDuration> intervals() const noexcept { return intervals_; }

    // Call at initialization; also usable to inspect the current deadlines.
    // Requests strictly beyond horizon are omitted without adding overflowing times.
    [[nodiscard]] std::vector<TimeoutRequest> timer_requests(SimTime horizon) const;
    HeartbeatUpdate receive_heartbeat(SimTime at, const HeartbeatMessage& message, SimTime horizon);
    TimeoutUpdate receive_timeout(SimTime at, const TimeoutRequest& request);
private:
    void check_time(SimTime at) const;
    NodeId node_;
    TimeoutParameters parameters_;
    TimeoutPolicy policy_;
    NodeStatus status_ = NodeStatus::alive;
    SimTime last_contact_{};
    std::optional<SimTime> last_real_contact_;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t generation_ = 0;
    SimTime last_observation_{};
    std::vector<SimDuration> intervals_;
    DetectorThresholds thresholds_;
};
}  // namespace telemetry::core
