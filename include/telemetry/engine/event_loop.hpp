#pragma once

#include "telemetry/common/time.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

namespace telemetry::engine {
class Phase {
public:
    template<detail::Integer T>
    explicit constexpr Phase(T value)
        : value_(detail::checked_nonnegative<std::numeric_limits<std::uint64_t>::max()>(value)) {
        if (value_ == 0) { throw std::invalid_argument("Phase must be positive"); }
    }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    auto operator<=>(const Phase&) const = default;
private:
    std::uint64_t value_;
};

// Opaque components, compared lexicographically. Unused components are zero.
using TieKey = std::array<std::uint64_t, 4>;
struct EventKey {
    SimTime at;
    Phase phase;
    TieKey tie_key;
    std::uint64_t insertion_sequence;
    auto operator<=>(const EventKey&) const = default;
};
enum class RunState { ready, running, completed, failed };
struct RunResult {
    SimTime reached;
    std::uint64_t executed;
    bool operator==(const RunResult&) const = default;
};
class EventLimitExceeded : public std::runtime_error {
public:
    EventLimitExceeded() : std::runtime_error("Event limit reached before completion") {}
};

class EventLoop {
public:
    using Action = std::function<void(EventLoop&)>;
    // A zero horizon permits events at time zero. A zero event limit permits
    // successful completion only if there are no events to execute.
    explicit EventLoop(SimTime horizon) : horizon_(horizon) {}
    template<detail::Integer T>
    EventLoop(SimTime horizon, T max_events)
        : horizon_(horizon), max_events_(detail::checked_nonnegative<
              std::numeric_limits<std::uint64_t>::max()>(max_events)) {}
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    // nullopt means strictly beyond horizon: no queue entry or sequence spent.
    // During execution, same-time events require a strictly later phase.
    [[nodiscard]] std::optional<EventKey> schedule(SimTime at, Phase phase, TieKey tie_key, Action action);
    [[nodiscard]] std::optional<EventKey> schedule_after(SimDuration delay, Phase phase, TieKey tie_key, Action action);
    // One run per instance, including after error; callback exceptions propagate.
    [[nodiscard]] RunResult run();
    [[nodiscard]] SimTime now() const noexcept { return now_; }
    [[nodiscard]] SimTime horizon() const noexcept { return horizon_; }
    [[nodiscard]] RunState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t executed() const noexcept { return executed_; }
    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }
    // Last invoked action's key; also identifies an action that threw.
    [[nodiscard]] const std::optional<EventKey>& last_event() const noexcept { return last_event_; }
private:
    struct Event {
        EventKey key;
        Action action;
    };
    struct Later {
        bool operator()(const Event& a, const Event& b) const noexcept { return a.key > b.key; }
    };
    void check_schedule(const Action& action) const;
    SimTime horizon_;
    std::optional<std::uint64_t> max_events_;
    SimTime now_{};
    RunState state_ = RunState::ready;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t executed_ = 0;
    std::optional<EventKey> last_event_;
    std::vector<Event> queue_;
};
}  // namespace telemetry::engine
