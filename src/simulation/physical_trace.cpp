#include "telemetry/simulation/physical_trace.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace telemetry::simulation {
PhysicalTrace::PhysicalTrace(SimTime horizon, PhysicalInput input)
    : horizon_(horizon), input_(std::move(input)) {
    if (input_.heartbeat_period.ticks() == 0) { throw std::invalid_argument("Heartbeat period must be positive"); }
    std::sort(input_.nodes.begin(), input_.nodes.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::map<NodeId,std::uint64_t> counts;
    for (const auto& node : input_.nodes) {
        if (!std::isfinite(node.performance) || node.performance <= 0) { throw std::invalid_argument("Invalid node performance"); }
        if (!counts.emplace(node.id,0).second) { throw std::invalid_argument("Duplicate node ID"); }
    }
    std::sort(input_.crashes.begin(), input_.crashes.end(), [](const auto& a, const auto& b) { return a.node < b.node; });
    std::optional<NodeId> previous;
    for (const auto& crash : input_.crashes) {
        if (!counts.contains(crash.node) || crash.at > horizon_ || previous == crash.node) {
            throw std::invalid_argument("Unknown node, duplicate crash or crash beyond horizon");
        }
        previous = crash.node;
    }
    std::sort(input_.blackouts.begin(), input_.blackouts.end(), [](const auto& a, const auto& b) {
        return std::tie(a.node,a.start,a.end) < std::tie(b.node,b.start,b.end);
    });
    std::vector<Blackout> merged;
    for (const auto& blackout : input_.blackouts) {
        if (!counts.contains(blackout.node) || blackout.start >= blackout.end) {
            throw std::invalid_argument("Unknown blackout node or invalid half-open interval");
        }
        if (!merged.empty() && merged.back().node == blackout.node && blackout.start <= merged.back().end) {
            merged.back().end = std::max(merged.back().end,blackout.end);
        } else { merged.push_back(blackout); }
    }
    input_.blackouts.swap(merged);
    std::sort(input_.deliveries.begin(), input_.deliveries.end(), [](const auto& a, const auto& b) {
        return std::tie(a.node,a.sequence) < std::tie(b.node,b.sequence);
    });
    const auto required = static_cast<std::uint64_t>(horizon_.ticks() / input_.heartbeat_period.ticks());
    const DeliveryRecord* last = nullptr;
    for (const auto& delivery : input_.deliveries) {
        if (!counts.contains(delivery.node) || delivery.sequence.value() > required ||
            (last && last->node == delivery.node && last->sequence == delivery.sequence)) {
            throw std::invalid_argument("Unknown node, duplicate or out-of-range delivery key");
        }
        auto& count = counts.at(delivery.node);
        count = checked_increment(count);
        last = &delivery;
    }
    // Unique positive sequence values in [1,required], plus this count, imply
    // completeness without iterating a potentially enormous horizon/H range.
    for (const auto& [id,count] : counts) {
        static_cast<void>(id);
        if (count != required) { throw std::invalid_argument("Incomplete delivery trace"); }
    }
}
const DeliveryRecord& PhysicalTrace::delivery(NodeId node, HeartbeatSeq sequence) const {
    const auto key = std::pair{node,sequence};
    const auto it = std::lower_bound(input_.deliveries.begin(),input_.deliveries.end(),key,
        [](const auto& record,const auto& value) { return std::pair{record.node,record.sequence} < value; });
    if (it == input_.deliveries.end() || it->node != node || it->sequence != sequence) {
        throw std::out_of_range("Delivery key absent from trace");
    }
    return *it;
}
bool PhysicalTrace::blocked(NodeId node, SimTime at) const {
    const auto key = std::pair{node,at};
    const auto it = std::upper_bound(input_.blackouts.begin(),input_.blackouts.end(),key,
        [](const auto& value,const auto& interval) { return value < std::pair{interval.node,interval.start}; });
    if (it == input_.blackouts.begin()) { return false; }
    const auto& interval = *std::prev(it);
    return interval.node == node && interval.start <= at && at < interval.end;
}
}  // namespace telemetry::simulation
