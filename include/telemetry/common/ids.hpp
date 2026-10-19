#pragma once

#include "telemetry/common/checked_integer.hpp"
#include <compare>

namespace telemetry {
template<class Tag>
class PositiveId {
public:
    template<detail::Integer T>
    explicit constexpr PositiveId(T value)
        : value_(detail::checked_nonnegative<std::numeric_limits<std::uint64_t>::max()>(value)) {
        if (value_ == 0) {
            throw std::invalid_argument("ID must be positive");
        }
    }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr PositiveId next() const { return PositiveId{checked_increment(value_)}; }
    auto operator<=>(const PositiveId&) const = default;
private:
    std::uint64_t value_;
};

using NodeId = PositiveId<struct NodeIdTag>;
using TaskId = PositiveId<struct TaskIdTag>;
using HeartbeatSeq = PositiveId<struct HeartbeatSeqTag>;
using AttemptNo = PositiveId<struct AttemptNoTag>;
}  // namespace telemetry
