#pragma once

#include "telemetry/common/messages.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace telemetry::core {
struct NodeLoad {
    NodeDefinition definition;
    long double sum = 0;
    long double compensation = 0;
    std::uint64_t assignments = 0;
    [[nodiscard]] long double assigned_work() const noexcept { return sum + compensation; }
    [[nodiscard]] long double load() const noexcept { return assigned_work() / definition.performance; }
};

class LoadLedger {
public:
    explicit LoadLedger(std::vector<NodeDefinition> nodes);
    [[nodiscard]] const NodeLoad& at(NodeId id) const;
    [[nodiscard]] std::span<const NodeLoad> nodes() const noexcept { return nodes_; }
    void preview_add(NodeId node, long double work) const;
    void preview_remove(NodeId node, long double work) const;
    void add(NodeId node, long double work);
    void remove(NodeId node, long double work);
    // Caller must revoke every current assignment of this node in the same batch.
    void clear_assignments(NodeId node);
    void swap(LoadLedger& other) noexcept;
private:
    struct Hash { std::size_t operator()(NodeId id) const noexcept { return std::hash<std::uint64_t>{}(id.value()); } };
    [[nodiscard]] static NodeLoad updated(NodeLoad value, long double work, bool remove);
    std::vector<NodeLoad> nodes_; // Canonical iteration order, ascending ID.
    std::unordered_map<NodeId, std::size_t, Hash> indices_; // Lookup only, never iteration order.
};

class RoundRobin {
public:
    explicit RoundRobin(std::vector<NodeId> original_ids);
    void update_alive(std::vector<NodeId> alive);
    [[nodiscard]] std::optional<NodeId> peek() const noexcept;
    void advance(); // Only after a committed assignment.
    void swap(RoundRobin& other) noexcept;
private:
    std::vector<NodeId> original_;
    std::vector<NodeId> alive_;
    std::optional<NodeId> next_;
    std::size_t position_ = 0;
};

class LeastLoaded {
public:
    [[nodiscard]] static std::optional<NodeId> select(std::span<const NodeId> alive, const LoadLedger& loads);
};
}  // namespace telemetry::core
