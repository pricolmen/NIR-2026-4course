#pragma once

#include <concepts>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace telemetry::detail {
template<class T>
concept Integer = std::integral<T> && !std::same_as<T, bool>;

// Check before converting: negative inputs must never become large unsigned IDs.
template<std::uint64_t Maximum, Integer T>
constexpr std::uint64_t checked_nonnegative(T value) {
    if constexpr (std::is_signed_v<T>) {
        if (value < 0) {
            throw std::invalid_argument("Integer must be nonnegative");
        }
    }
    if constexpr (std::numeric_limits<T>::digits > 63) {
        if (value > static_cast<T>(Maximum)) {
            throw std::overflow_error("Integer exceeds representable range");
        }
    }
    return static_cast<std::uint64_t>(value);
}
}  // namespace telemetry::detail

namespace telemetry {
template<detail::Integer T>
[[nodiscard]] constexpr std::uint64_t checked_increment(T input) {
    const auto value = detail::checked_nonnegative<std::numeric_limits<std::uint64_t>::max()>(input);
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Counter exhausted");
    }
    return value + 1;
}
}  // namespace telemetry
