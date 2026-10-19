#include "telemetry/common/time.hpp"

#include <algorithm>
#include <cmath>

namespace telemetry {
SimDuration ceil_ticks(long double raw_ticks) {
    if (!std::isfinite(raw_ticks) || raw_ticks < 0) {
        throw std::invalid_argument("Computed ticks must be finite and nonnegative");
    }
    const auto rounded = std::ceil(raw_ticks);
    // INT64_MAX rounds UP to 2^63 when long double is binary64 (Apple arm64).
    // Compare with the exactly representable exclusive bound before casting.
    if (rounded >= std::ldexp(1.0L, 63)) {
        throw std::overflow_error("Computed duration exceeds int64 range");
    }
    return SimDuration{static_cast<std::int64_t>(rounded)};
}

SimDuration processing_duration(long double work, long double performance) {
    if (!std::isfinite(work) || work <= 0 || !std::isfinite(performance) || performance <= 0) {
        throw std::invalid_argument("Work and performance must be finite and positive");
    }
    const auto duration_seconds = work / performance;
    const auto raw_ticks = duration_seconds * static_cast<long double>(ticks_per_second);
    if (!std::isfinite(duration_seconds) || !std::isfinite(raw_ticks)) {
        throw std::overflow_error("Processing duration is not finite");
    }
    if (duration_seconds <= 0 || raw_ticks <= 0) {
        throw std::underflow_error("Positive processing duration lost representability");
    }
    return std::max(SimDuration{1}, ceil_ticks(raw_ticks));
}

RoundedThresholds round_thresholds(long double raw_suspect, long double raw_failed) {
    if (!std::isfinite(raw_suspect) || raw_suspect <= 0 ||
        !std::isfinite(raw_failed) || raw_failed < raw_suspect) {
        throw std::invalid_argument("Thresholds must be finite, positive and ordered");
    }
    const auto suspect = ceil_ticks(raw_suspect);
    const auto failed = std::max(suspect + SimDuration{1}, ceil_ticks(raw_failed));
    return {raw_suspect, raw_failed, suspect, failed};
}
}  // namespace telemetry
