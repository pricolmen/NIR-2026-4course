#pragma once

#include "telemetry/common/messages.hpp"
#include <span>
#include <vector>

namespace telemetry::simulation {
struct Crash { NodeId node; SimTime at; };
struct Blackout { NodeId node; SimTime start; SimTime end; };
struct DeliveryRecord {
    NodeId node;
    HeartbeatSeq sequence;
    SimDuration delay;
    bool lost;
};
struct PhysicalInput {
    SimDuration heartbeat_period;
    std::vector<NodeDefinition> nodes;
    std::vector<Crash> crashes;
    std::vector<Blackout> blackouts;
    std::vector<DeliveryRecord> deliveries;
};

// Validated immutable in-memory input, shared by independent variant states.
class PhysicalTrace {
public:
    PhysicalTrace(SimTime horizon, PhysicalInput input);
    PhysicalTrace(const PhysicalTrace&) = default;
    PhysicalTrace& operator=(const PhysicalTrace&) = delete;
    PhysicalTrace& operator=(PhysicalTrace&&) = delete;
    [[nodiscard]] SimTime horizon() const noexcept { return horizon_; }
    [[nodiscard]] SimDuration heartbeat_period() const noexcept { return input_.heartbeat_period; }
    [[nodiscard]] std::span<const NodeDefinition> nodes() const noexcept { return input_.nodes; }
    [[nodiscard]] std::span<const Crash> crashes() const noexcept { return input_.crashes; }
    [[nodiscard]] std::span<const Blackout> blackouts() const noexcept { return input_.blackouts; }
    [[nodiscard]] std::span<const DeliveryRecord> deliveries() const noexcept { return input_.deliveries; }
    [[nodiscard]] const DeliveryRecord& delivery(NodeId node, HeartbeatSeq sequence) const;
    [[nodiscard]] bool blocked(NodeId node, SimTime at) const;
private:
    SimTime horizon_;
    PhysicalInput input_;
};
}  // namespace telemetry::simulation
