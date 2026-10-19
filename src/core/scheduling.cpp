#include "telemetry/core/scheduling.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace telemetry::core {
LoadLedger::LoadLedger(std::vector<NodeDefinition> nodes) {
    std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const auto& node : nodes) {
        if (!std::isfinite(node.performance) || node.performance <= 0) {
            throw std::invalid_argument("Node performance must be finite and positive");
        }
        if (indices_.contains(node.id)) { throw std::invalid_argument("Duplicate node ID"); }
        indices_.emplace(node.id, nodes_.size());
        nodes_.push_back({node});
    }
}

const NodeLoad& LoadLedger::at(NodeId id) const { return nodes_.at(indices_.at(id)); }

NodeLoad LoadLedger::updated(NodeLoad value, long double work, bool remove) {
    if (!std::isfinite(work) || work <= 0) { throw std::invalid_argument("Load work must be finite and positive"); }
    if (remove) {
        if (value.assignments == 0) { throw std::logic_error("Cannot remove from empty load"); }
        --value.assignments;
        if (value.assignments == 0) {
            value.sum = 0; value.compensation = 0;
            return value;
        }
    } else { value.assignments = checked_increment(value.assignments); }
    const auto delta = remove ? -work : work;
    const auto next_sum = value.sum + delta;
    if (!std::isfinite(next_sum)) { throw std::overflow_error("Load sum overflow"); }
    // Neumaier compensation preserves a small assignment across cancellation
    // when a much larger assignment is removed later.
    const auto error = std::abs(value.sum) >= std::abs(delta)
        ? (value.sum - next_sum) + delta : (delta - next_sum) + value.sum;
    const auto correction = value.compensation + error;
    const auto total = next_sum + correction;
    const auto load = total / value.definition.performance;
    if (!std::isfinite(error) || !std::isfinite(correction) || !std::isfinite(total) || !std::isfinite(load)) {
        throw std::overflow_error("Load arithmetic is not finite");
    }
    if (total <= 0 || load <= 0) { throw std::underflow_error("Positive load lost representability"); }
    value.sum = next_sum; value.compensation = correction;
    return value;
}

void LoadLedger::preview_add(NodeId node, long double work) const { static_cast<void>(updated(at(node), work, false)); }
void LoadLedger::preview_remove(NodeId node, long double work) const { static_cast<void>(updated(at(node), work, true)); }
void LoadLedger::add(NodeId node, long double work) { nodes_.at(indices_.at(node)) = updated(at(node), work, false); }
void LoadLedger::remove(NodeId node, long double work) { nodes_.at(indices_.at(node)) = updated(at(node), work, true); }

void LoadLedger::clear_assignments(NodeId node) {
    auto& value = nodes_.at(indices_.at(node));
    value.sum = 0; value.compensation = 0; value.assignments = 0;
}
void LoadLedger::swap(LoadLedger& other) noexcept {
    nodes_.swap(other.nodes_);
    indices_.swap(other.indices_);
}
void RoundRobin::swap(RoundRobin& other) noexcept {
    original_.swap(other.original_);
    alive_.swap(other.alive_);
    next_.swap(other.next_);
    std::swap(position_, other.position_);
}

RoundRobin::RoundRobin(std::vector<NodeId> original_ids) : original_(std::move(original_ids)) {
    std::sort(original_.begin(), original_.end());
    if (std::adjacent_find(original_.begin(), original_.end()) != original_.end()) {
        throw std::invalid_argument("Duplicate Round Robin node ID");
    }
    update_alive(original_);
}

void RoundRobin::update_alive(std::vector<NodeId> alive) {
    std::sort(alive.begin(), alive.end());
    if (std::adjacent_find(alive.begin(), alive.end()) != alive.end()) { throw std::invalid_argument("Duplicate ALIVE ID"); }
    for (auto id : alive) {
        if (!std::binary_search(original_.begin(), original_.end(), id)) { throw std::invalid_argument("Unknown ALIVE ID"); }
    }
    auto position = std::size_t{0};
    auto next = next_;
    if (!alive.empty()) {
        const auto found = next ? std::lower_bound(alive.begin(), alive.end(), *next) : alive.begin();
        position = found == alive.end() ? 0 : static_cast<std::size_t>(found - alive.begin());
        next = alive[position];
    }
    alive_ = std::move(alive); position_ = position; next_ = next;
}

std::optional<NodeId> RoundRobin::peek() const noexcept {
    if (alive_.empty()) { return std::nullopt; }
    return alive_[position_];
}
void RoundRobin::advance() {
    if (alive_.empty()) { throw std::logic_error("Cannot advance empty Round Robin"); }
    position_ = position_ + 1 == alive_.size() ? 0 : position_ + 1;
    next_ = alive_[position_];
}

std::optional<NodeId> LeastLoaded::select(std::span<const NodeId> alive, const LoadLedger& loads) {
    std::optional<NodeId> best;
    long double best_load = 0;
    for (const auto id : alive) {
        const auto load = loads.at(id).load();
        if (!best || load < best_load || (load == best_load && id < *best)) { best = id; best_load = load; }
    }
    return best;
}
}  // namespace telemetry::core
