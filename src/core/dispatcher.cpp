#include "telemetry/core/dispatcher.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace telemetry::core {
namespace {
std::vector<NodeId> node_ids(const LoadLedger& loads) {
    std::vector<NodeId> ids;
    for (const auto& node : loads.nodes()) { ids.push_back(node.definition.id); }
    return ids;
}
struct Operation {
    bool& active;
    explicit Operation(bool& flag) : active(flag) {
        if (active) { throw std::logic_error("Dispatcher mutation during another operation"); }
        active = true;
    }
    ~Operation() { active = false; }
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
};
}

Dispatcher::Dispatcher(std::vector<NodeDefinition> nodes, SchedulingPolicy policy, AuditSink* sink)
    : loads_(std::move(nodes)), alive_(node_ids(loads_)), round_robin_(alive_), tasks_(this), policy_(policy), sink_(sink) {
    if (policy != SchedulingPolicy::round_robin && policy != SchedulingPolicy::least_loaded) {
        throw std::invalid_argument("Unknown scheduling policy");
    }
}

void Dispatcher::check_time(SimTime at) const {
    if (last_observation_ && at < *last_observation_) { throw std::invalid_argument("Dispatcher time moved backwards"); }
}

void Dispatcher::arrive(SimTime at, TaskDescription task) {
    Operation operation{operating_}; check_time(at);
    tasks_.arrive(at, task);
}

void Dispatcher::set_alive(SimTime at, std::vector<NodeId> alive) {
    static_cast<void>(apply_availability(at, std::move(alive), {}));
}

ReconfigurationEvent Dispatcher::apply_availability(
    SimTime at, std::vector<NodeId> alive, std::vector<NodeId> failed) {
    Operation operation{operating_}; check_time(at);
    std::sort(alive.begin(), alive.end());
    std::sort(failed.begin(), failed.end());
    if (std::adjacent_find(failed.begin(), failed.end()) != failed.end()) {
        throw std::invalid_argument("Duplicate FAILED ID");
    }
    for (const auto node : failed) {
        try { static_cast<void>(loads_.at(node)); }
        catch (const std::out_of_range&) { throw std::invalid_argument("Unknown FAILED ID"); }
    }
    std::vector<NodeId> newly_failed;
    std::set_difference(failed.begin(), failed.end(), failed_.begin(), failed_.end(),
                        std::back_inserter(newly_failed));
    std::vector<NodeId> all_failed;
    std::set_union(failed.begin(), failed.end(), failed_.begin(), failed_.end(),
                   std::back_inserter(all_failed));
    for (const auto node : alive) {
        if (std::binary_search(all_failed.begin(), all_failed.end(), node)) {
            throw std::invalid_argument("FAILED node cannot be ALIVE");
        }
    }
    auto next_rr = round_robin_;
    next_rr.update_alive(alive); // Validate/preallocate once for the final set.
    auto next_loads = loads_;
    for (const auto node : newly_failed) { next_loads.clear_assignments(node); }
    auto revocations = tasks_.prepare_revocations(at, newly_failed);
    ReconfigurationEvent report{at, std::move(alive), std::move(newly_failed), {}};
    report.revoked.reserve(revocations.tasks.size());
    for (const auto* task : revocations.tasks) {
        report.revoked.push_back({task->description.id, *task->current_attempt, task->attempts.back().node});
    }
    AuditEvent event{report}; // Allocate journal payload before logical commit.
    // From here to on_event every operation is nonthrowing; callbacks observe
    // the entire batch, including exact zero loads for all newly FAILED nodes.
    alive_.swap(report.alive);
    failed_.swap(all_failed);
    round_robin_.swap(next_rr);
    loads_.swap(next_loads);
    tasks_.commit_revocations(at, revocations);
    last_observation_ = at;
    on_event(event);
    return std::get<ReconfigurationEvent>(std::move(event));
}

std::vector<AssignmentCommand> Dispatcher::dispatch(SimTime at) {
    Operation operation{operating_}; check_time(at);
    std::vector<AssignmentCommand> commands;
    if (!alive_.empty()) { commands.reserve(tasks_.pending().size()); }
    while (!tasks_.pending().empty() && !alive_.empty()) {
        const auto node = policy_ == SchedulingPolicy::round_robin
            ? round_robin_.peek() : LeastLoaded::select(alive_, loads_);
        if (!node) { throw std::logic_error("No strategy choice for nonempty ALIVE set"); }
        const auto& task = tasks_.tasks().at(tasks_.pending().front()).description;
        loads_.preview_add(*node, task.work);
        commands.push_back(*tasks_.assign_next(at, *node));
    }
    last_observation_ = at;
    return commands;
}

ResultDecision Dispatcher::receive_result(SimTime at, ResultMessage result) {
    Operation operation{operating_}; check_time(at);
    if (tasks_.inspect_result(result) == ResultDecision::accepted) {
        loads_.preview_remove(result.node, tasks_.tasks().at(result.task).description.work);
    }
    return tasks_.receive_result(at, result);
}

bool Dispatcher::revoke_assignment(SimTime at, TaskId id) {
    Operation operation{operating_}; check_time(at);
    const auto& task = tasks_.tasks().at(id);
    if (task.current_attempt) { loads_.preview_remove(task.attempts.back().node, task.description.work); }
    const auto revoked = tasks_.revoke_assignment(at, id);
    last_observation_ = at;
    return revoked;
}

void Dispatcher::on_event(const AuditEvent& event) {
    std::visit([this](const auto& value) {
        using Event = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Event, TaskAssignedEvent>) {
            loads_.add(value.assignment.node, value.assignment.task.work);
            if (policy_ == SchedulingPolicy::round_robin) { round_robin_.advance(); }
        } else if constexpr (std::is_same_v<Event, AssignmentRevokedEvent>) {
            loads_.remove(value.assignment.node, tasks_.tasks().at(value.assignment.task).description.work);
        } else if constexpr (std::is_same_v<Event, ResultEvent>) {
            if (value.decision == ResultDecision::accepted) {
                loads_.remove(value.result.node, tasks_.tasks().at(value.result.task).description.work);
            }
        }
        last_observation_ = value.at;
    }, event);
    if (sink_) { sink_->on_event(event); }
}

void Dispatcher::validate_invariants() const {
    tasks_.validate_invariants();
    if (!std::is_sorted(alive_.begin(), alive_.end()) ||
        std::adjacent_find(alive_.begin(), alive_.end()) != alive_.end() ||
        !std::is_sorted(failed_.begin(), failed_.end()) ||
        std::adjacent_find(failed_.begin(), failed_.end()) != failed_.end()) {
        throw std::logic_error("Availability sets must be sorted and unique");
    }
    for (const auto node : alive_) {
        static_cast<void>(loads_.at(node));
        if (std::binary_search(failed_.begin(), failed_.end(), node)) {
            throw std::logic_error("FAILED node is ALIVE");
        }
    }
    for (const auto node : failed_) {
        if (loads_.at(node).assignments != 0 || loads_.at(node).assigned_work() != 0) {
            throw std::logic_error("FAILED node retains logical load");
        }
    }
    for (const auto& [id, task] : tasks_.tasks()) {
        static_cast<void>(id);
        if (task.current_attempt) { static_cast<void>(loads_.at(task.attempts.back().node)); }
    }
    for (const auto& node : loads_.nodes()) {
        long double sum = 0;
        std::uint64_t count = 0;
        for (const auto& [id, task] : tasks_.tasks()) {
            static_cast<void>(id);
            if (task.current_attempt && task.attempts.back().node == node.definition.id) {
                sum += task.description.work;
                count = checked_increment(count);
            }
        }
        const auto tolerance = 64 * std::numeric_limits<long double>::epsilon()
            * std::max(1.0L, static_cast<long double>(count)) * std::max(1.0L, sum);
        if (!std::isfinite(sum) || !std::isfinite(tolerance)) { throw std::overflow_error("Load audit overflow"); }
        if (!std::isfinite(node.assigned_work()) || !std::isfinite(node.load()) ||
            (count > 0 && (node.assigned_work() <= 0 || node.load() <= 0)) ||
            count != node.assignments || (count == 0 && node.assigned_work() != 0) ||
            std::abs(node.assigned_work() - sum) > tolerance) {
            throw std::logic_error("Load counter differs from current assignments");
        }
    }
}
}  // namespace telemetry::core
