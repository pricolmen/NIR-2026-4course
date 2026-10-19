#pragma once

#include "telemetry/common/checked_integer.hpp"
#include <compare>
#include <optional>

namespace telemetry {
inline constexpr std::int64_t ticks_per_second = 1'000'000;
inline constexpr std::int64_t tick_ns = 1'000;

class SimDuration {
public:
    constexpr SimDuration() = default;
    template<detail::Integer T>
    explicit constexpr SimDuration(T ticks)
        : ticks_(static_cast<std::int64_t>(detail::checked_nonnegative<
              static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())>(ticks))) {}
    [[nodiscard]] constexpr std::int64_t ticks() const noexcept { return ticks_; }
    auto operator<=>(const SimDuration&) const = default;
private:
    std::int64_t ticks_ = 0;
};

class SimTime {
public:
    constexpr SimTime() = default;
    template<detail::Integer T>
    explicit constexpr SimTime(T ticks)
        : ticks_(static_cast<std::int64_t>(detail::checked_nonnegative<
              static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())>(ticks))) {}
    [[nodiscard]] constexpr std::int64_t ticks() const noexcept { return ticks_; }
    auto operator<=>(const SimTime&) const = default;
private:
    std::int64_t ticks_ = 0;
};

[[nodiscard]] constexpr SimDuration operator+(SimDuration a, SimDuration b) {
    if (b.ticks() > std::numeric_limits<std::int64_t>::max() - a.ticks()) {
        throw std::overflow_error("Duration addition overflow");
    }
    return SimDuration{a.ticks() + b.ticks()};
}
[[nodiscard]] constexpr SimDuration operator-(SimDuration a, SimDuration b) {
    if (a < b) { throw std::invalid_argument("Negative duration difference"); }
    return SimDuration{a.ticks() - b.ticks()};
}
[[nodiscard]] constexpr SimTime operator+(SimTime time, SimDuration duration) {
    return SimTime{(SimDuration{time.ticks()} + duration).ticks()};
}
[[nodiscard]] constexpr SimTime operator-(SimTime time, SimDuration duration) {
    return SimTime{(SimDuration{time.ticks()} - duration).ticks()};
}
[[nodiscard]] constexpr SimDuration operator-(SimTime end, SimTime start) {
    return SimDuration{end.ticks()} - SimDuration{start.ticks()};
}

// nullopt means strictly beyond the horizon, not arithmetic overflow.
[[nodiscard]] constexpr std::optional<SimTime> deadline_within(
    SimTime now, SimDuration delay, SimTime horizon) {
    if (now > horizon) { throw std::invalid_argument("Current time exceeds horizon"); }
    if (delay > horizon - now) { return std::nullopt; }
    return now + delay;
}
[[nodiscard]] constexpr long double seconds(SimDuration duration) noexcept {
    return static_cast<long double>(duration.ticks()) / static_cast<long double>(ticks_per_second);
}

struct RoundedThresholds {
    long double raw_suspect_ticks;
    long double raw_failed_ticks;
    SimDuration suspect;
    SimDuration failed;
};

[[nodiscard]] SimDuration ceil_ticks(long double raw_ticks);
[[nodiscard]] SimDuration processing_duration(long double work, long double performance);
// Only discretizes already computed thresholds; detector policies are stage 5.
[[nodiscard]] RoundedThresholds round_thresholds(long double raw_suspect, long double raw_failed);
}  // namespace telemetry
