#include "telemetry/metrics/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace telemetry::metrics {
namespace {
using namespace simulation;
void require(bool condition) { if (!condition) { throw std::invalid_argument("Inconsistent simulation journal"); } }
ResultMessage message(const AssignmentCommand& command) { return {command.task.id,command.attempt,command.node}; }
void sample(std::optional<long double>& mean,std::uint64_t& count,SimDuration duration) {
    count = checked_increment(count);
    const auto value = static_cast<long double>(duration.ticks());
    mean = mean.value_or(0) + (value-mean.value_or(0))/static_cast<long double>(count);
    if (!std::isfinite(*mean)) { throw std::overflow_error("Metric mean overflow"); }
}
}
std::optional<std::size_t> p95_rank(std::size_t count) noexcept {
    return count ? std::optional<std::size_t>{count-count/20} : std::nullopt;
}
Analyzer::Analyzer(std::shared_ptr<const simulation::Scenario> scenario) : scenario_(std::move(scenario)) {
    if (!scenario_) { throw std::invalid_argument("Analyzer requires a scenario"); }
    for (const auto& task : scenario_->tasks()) { report_.tasks.emplace(task.id,TaskReport{task,{},{},0}); }
    for (const auto& node : scenario_->physical()->nodes()) {
        report_.nodes.emplace(node.id,NodeReport{node.id,{},{},core::NodeStatus::alive,{}, {},{}, {}});
        waiting_.emplace(node.id,std::deque<PhysicalAttemptKey>{}); running_.emplace(node.id,std::nullopt);
    }
    for (const auto& crash : scenario_->physical()->crashes()) { report_.nodes.at(crash.node).crash_at = crash.at; }
}
AttemptReport& Analyzer::attempt(ResultMessage result) {
    auto& value = report_.attempts.at({result.task,result.attempt});
    require(message(value.assignment) == result); return value;
}
void Analyzer::revoke(SimTime at,ResultMessage result) {
    auto& value = attempt(result); auto& task = report_.tasks.at(result.task);
    require(task.current == result.attempt && !task.completed_at && !value.revoked_at && !value.accepted_at);
    value.revoked_at = at; task.current.reset(); pending_.push_back(result.task);
}
void Analyzer::logical(const core::AuditEvent& event) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T,core::TaskArrivedEvent>) {
            require(last_->phase == phases::arrival && report_.tasks.at(value.task.id).description == value.task &&
                value.at == value.task.arrival && arrived_.insert(value.task.id).second);
            pending_.push_back(value.task.id);
        } else if constexpr (std::is_same_v<T,core::TaskAssignedEvent>) {
            require(last_->phase == phases::dispatch);
            const auto& cmd = value.assignment; auto& task = report_.tasks.at(cmd.task.id);
            require(task.description == cmd.task && arrived_.contains(cmd.task.id) && !task.current && !task.completed_at &&
                !pending_.empty() && pending_.front() == cmd.task.id && cmd.attempt.value() == checked_increment(task.attempts) &&
                report_.nodes.at(cmd.node).status == core::NodeStatus::alive);
            commands_ = checked_increment(commands_);
            pending_.pop_front(); task.current = cmd.attempt; task.attempts = cmd.attempt.value();
            require(report_.attempts.emplace(PhysicalAttemptKey{cmd.task.id,cmd.attempt},AttemptReport{cmd,value.at,false,{},{},{},{},{},{}}).second);
            if (task.attempts > 1) { report_.summary.retries = checked_increment(report_.summary.retries); }
        } else if constexpr (std::is_same_v<T,core::AssignmentRevokedEvent>) {
            // The integrated runner uses a coherent ReconfigurationEvent only.
            throw std::invalid_argument("Standalone revocation is not an integrated journal event");
        } else if constexpr (std::is_same_v<T,core::ResultEvent>) {
            require(last_->phase == phases::completion);
            auto& value_attempt = attempt(value.result); auto& task = report_.tasks.at(value.result.task);
            require(value_attempt.finished_at == value.at && !value_attempt.result);
            const auto expected = task.completed_at ? core::ResultDecision::already_completed :
                !task.current ? core::ResultDecision::no_current_assignment :
                *task.current != value.result.attempt ? core::ResultDecision::attempt_mismatch :
                report_.attempts.at({value.result.task,*task.current}).assignment.node != value.result.node ? core::ResultDecision::node_mismatch :
                core::ResultDecision::accepted;
            require(value.decision == expected); value_attempt.result = value.decision;
            if (value.decision == core::ResultDecision::accepted) {
                require(!value_attempt.revoked_at); value_attempt.accepted_at = value.at;
                task.completed_at = value.at; task.current.reset();
            } else if (value_attempt.revoked_at) { report_.summary.rejected_revoked_results = checked_increment(report_.summary.rejected_revoked_results); }
        } else if constexpr (std::is_same_v<T,core::ReconfigurationEvent>) {
            require(last_->phase == phases::reconfiguration && last_reconfiguration_ != value.at);
            last_reconfiguration_ = value.at;
            require(value.newly_failed == std::vector<NodeId>{pending_failed_.begin(),pending_failed_.end()});
            std::vector<NodeId> alive;
            for (const auto& [id,node] : report_.nodes) { if (node.status == core::NodeStatus::alive) { alive.push_back(id); } }
            require(alive == value.alive);
            std::vector<ResultMessage> expected;
            for (const auto& [id,task] : report_.tasks) {
                if (task.current) {
                    const auto& cmd = report_.attempts.at({id,*task.current}).assignment;
                    if (pending_failed_.contains(cmd.node)) { expected.push_back(message(cmd)); }
                }
            }
            std::sort(expected.begin(),expected.end(),[&](auto a,auto b) {
                const auto aa = report_.tasks.at(a.task).description.arrival, bb = report_.tasks.at(b.task).description.arrival;
                return aa != bb ? aa < bb : a.task < b.task;
            });
            require(value.revoked == expected);
            for (auto id : pending_failed_) { require(configured_failed_.insert(id).second); }
            for (auto result : value.revoked) { report_.nodes.at(result.node).returned.push_back(result.task); revoke(value.at,result); }
            pending_failed_.clear();
        }
    },event);
}
void Analyzer::physical(const simulation::PhysicalEvent& event) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T,PhysicalAssigned>) {
            require(last_->phase == phases::dispatch);
            auto& item = attempt(message(value.assignment));
            require(item.assignment == value.assignment && item.assigned_at == value.at && !item.physical_assigned);
            item.physical_assigned = true; waiting_.at(value.assignment.node).push_back({value.assignment.task.id,value.assignment.attempt});
        } else if constexpr (std::is_same_v<T,PhysicalStarted>) {
            require(last_->phase == phases::dispatch || last_->phase == phases::completion);
            auto& item = attempt(value.attempt); auto& queue = waiting_.at(value.attempt.node); auto& running = running_.at(value.attempt.node);
            const PhysicalAttemptKey key{value.attempt.task,value.attempt.attempt};
            require(item.physical_assigned && !item.started_at && !crashed_.contains(value.attempt.node) && !running &&
                !queue.empty() && queue.front() == key);
            const auto node = std::find_if(scenario_->physical()->nodes().begin(),scenario_->physical()->nodes().end(),[&](auto n) { return n.id == value.attempt.node; });
            require(value.duration == processing_duration(item.assignment.task.work,node->performance) &&
                value.completion_due == deadline_within(value.at,value.duration,scenario_->physical()->horizon()) && value.token > 0);
            queue.pop_front(); running = key; item.started_at = value.at;
        } else if constexpr (std::is_same_v<T,PhysicalFinished>) {
            require(last_->phase == phases::completion);
            auto& item = attempt(value.attempt); auto& running = running_.at(value.attempt.node);
            require(running == PhysicalAttemptKey{value.attempt.task,value.attempt.attempt} && item.started_at && !item.finished_at &&
                !item.interrupted_at && !crashed_.contains(value.attempt.node));
            const auto node = std::find_if(scenario_->physical()->nodes().begin(),scenario_->physical()->nodes().end(),[&](auto n) { return n.id == value.attempt.node; });
            require(value.at-*item.started_at == processing_duration(item.assignment.task.work,node->performance));
            item.finished_at = value.at; running.reset();
        } else if constexpr (std::is_same_v<T,PhysicalCrash>) {
            require(last_->phase == phases::crash && report_.nodes.at(value.node).crash_at == value.at && crashed_.insert(value.node).second);
            auto& running = running_.at(value.node);
            require(value.interrupted.has_value() == running.has_value());
            if (value.interrupted) {
                require(PhysicalAttemptKey{value.interrupted->task,value.interrupted->attempt} == *running && value.interrupted->node == value.node);
                attempt(*value.interrupted).interrupted_at = value.at;
            }
            running.reset();
        } else if constexpr (std::is_same_v<T,PhysicalCompletionSkipped>) {
            require(last_->phase == phases::completion && crashed_.contains(value.attempt.node) && !attempt(value.attempt).finished_at);
        } else if constexpr (std::is_same_v<T,PhysicalHeartbeatSent> || std::is_same_v<T,PhysicalSendSkipped>) {
            require(last_->phase == phases::heartbeat_send); static_cast<void>(report_.nodes.at(value.message.node));
        } else if constexpr (std::is_same_v<T,PhysicalHeartbeatDelivered>) {
            require(last_->phase == phases::heartbeat_receive); static_cast<void>(report_.nodes.at(value.message.node));
        }
    },event);
}
void Analyzer::transition(NodeId id,core::NodeStatus before,core::NodeStatus after,SimTime at) {
    auto& node = report_.nodes.at(id); require(node.status == before && (before != core::NodeStatus::failed || after == before));
    node.status = after;
    if (after == core::NodeStatus::failed && before != after) { require(!node.failed_at); node.failed_at = at; pending_failed_.insert(id); }
}
void Analyzer::on_event(const engine::EventKey& key,const simulation::SimulationEvent& event) {
    if (finished_ || failed_) { throw std::logic_error("Analyzer is closed"); }
    try {
        require(key.at <= scenario_->physical()->horizon() && key.phase.value() <= 8 && key.insertion_sequence > 0 && (!last_ || *last_ <= key));
        if (!last_ || *last_ != key) { actions_ = checked_increment(actions_); }
        last_ = key;
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T,core::AuditEvent> || std::is_same_v<T,PhysicalEvent>) {
                std::visit([&](const auto& nested) { require(nested.at == key.at); },value);
                if constexpr (std::is_same_v<T,core::AuditEvent>) { logical(value); } else { physical(value); }
            } else {
                require(value.at == key.at);
                if constexpr (std::is_same_v<T,HeartbeatObserved>) {
                    require(key.phase == phases::heartbeat_receive); transition(value.message.node,value.update.before,value.update.after,value.at);
                } else if constexpr (std::is_same_v<T,TimeoutObserved>) {
                    require(key.phase == phases::timeout && value.request.at == key.at); transition(value.request.node,value.update.before,value.update.after,value.at);
                } else {
                    require(key.phase == phases::dispatch && last_dispatch_ != key.at); last_dispatch_ = key.at;
                    require(value.commands == commands_); commands_ = 0;
                    require(pending_.empty() || std::none_of(report_.nodes.begin(),report_.nodes.end(),[](const auto& pair) { return pair.second.status == core::NodeStatus::alive; }));
                }
            }
        },event);
    } catch (...) { failed_ = true; throw; }
}
Analysis Analyzer::finish(engine::RunResult result) {
    if (finished_ || failed_) { throw std::logic_error("Analyzer is closed"); }
    try {
        require(result.executed >= actions_ && commands_ == 0 && result.reached == scenario_->physical()->horizon() && (!last_ || last_->at <= result.reached) && pending_failed_.empty());
        require(arrived_.size() == report_.tasks.size());
        std::set<TaskId> pending{pending_.begin(),pending_.end()}; require(pending.size() == pending_.size());
        std::vector<SimDuration> latency;
        auto& s = report_.summary;
        for (const auto& [id,task] : report_.tasks) {
            s.arrived = checked_increment(s.arrived);
            require(task.completed_at ? !task.current && !pending.contains(id) : task.current.has_value() != pending.contains(id));
            if (task.completed_at) {
                const auto duration = *task.completed_at-task.description.arrival; latency.push_back(duration);
                sample(s.mean_latency_ticks,s.completed,duration);
            } else {
                s.unfinished = checked_increment(s.unfinished);
                if (task.current) { s.assigned = checked_increment(s.assigned); require(!configured_failed_.contains(report_.attempts.at({id,*task.current}).assignment.node)); }
                else { s.queued = checked_increment(s.queued); }
            }
        }
        require(s.arrived-s.completed == s.unfinished && s.unfinished-s.queued == s.assigned);
        for (const auto& [key,item] : report_.attempts) {
            static_cast<void>(key);
            require(item.physical_assigned && item.finished_at.has_value() == item.result.has_value());
        }
        std::sort(latency.begin(),latency.end());
        if (auto rank = p95_rank(latency.size())) { s.p95_latency = latency[*rank-1]; }
        for (const auto& window : scenario_->measurement_windows()) {
            std::uint64_t count = 0;
            for (const auto& [id,task] : report_.tasks) {
                static_cast<void>(id);
                if (task.completed_at && *task.completed_at > window.start && *task.completed_at <= window.end) { count = checked_increment(count); }
            }
            s.throughput.push_back({window,count,(static_cast<long double>(count)*static_cast<long double>(ticks_per_second))/static_cast<long double>((window.end-window.start).ticks())});
        }
        for (auto& [id,node] : report_.nodes) {
            require(node.crash_at.has_value() == crashed_.contains(id));
            if (node.crash_at) { s.crashes = checked_increment(s.crashes); }
            const bool false_positive = node.failed_at && (!node.crash_at || *node.failed_at < *node.crash_at);
            if (false_positive) { s.false_positives = checked_increment(s.false_positives); }
            if (false_positive && node.crash_at) {
                node.classification = "crash_after_false_exclusion"; s.crashes_after_false_exclusion = checked_increment(s.crashes_after_false_exclusion);
                node.detection = {{},"crash_after_false_exclusion"}; node.recovery = node.detection;
            } else if (false_positive) {
                node.classification = "false_positive"; node.detection = {{},"not_applicable"}; node.recovery = node.detection;
            } else if (node.crash_at && node.failed_at) {
                node.classification = "true_positive"; node.detection = {*node.failed_at-*node.crash_at,{}};
                sample(s.mean_detection_ticks,s.detection_samples,*node.detection.value);
                SimTime latest = *node.failed_at; bool complete = true;
                for (auto task_id : node.returned) {
                    const auto done = report_.tasks.at(task_id).completed_at;
                    if (!done) { complete = false; } else { latest = std::max(latest,*done); }
                }
                if (complete) { node.recovery = {latest-*node.crash_at,{}}; sample(s.mean_recovery_ticks,s.recovery_samples,*node.recovery.value); }
                else { node.recovery = {{},"recovery_incomplete"}; s.incomplete_recoveries = checked_increment(s.incomplete_recoveries); }
            } else if (node.crash_at) {
                node.classification = "not_detected"; s.false_negatives = checked_increment(s.false_negatives);
                node.detection = {{},"not_detected"}; node.recovery = node.detection;
            } else {
                node.classification = "no_failure"; node.detection = {{},"not_applicable"}; node.recovery = node.detection;
            }
        }
        finished_ = true; return std::move(report_);
    } catch (...) { failed_ = true; throw; }
}
}  // namespace telemetry::metrics
