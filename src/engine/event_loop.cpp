#include "telemetry/engine/event_loop.hpp"

#include <algorithm>
#include <utility>

namespace telemetry::engine {
void EventLoop::check_schedule(const Action& action) const {
    if (state_ != RunState::ready && state_ != RunState::running) {
        throw std::logic_error("Cannot schedule after a completed or failed run");
    }
    if (!action) { throw std::invalid_argument("Event action must not be empty"); }
}

std::optional<EventKey> EventLoop::schedule(SimTime at, Phase phase, TieKey tie_key, Action action) {
    check_schedule(action);
    if (at < now_) { throw std::invalid_argument("Cannot schedule an event in the past"); }
    if (state_ == RunState::running && at == now_ && phase <= last_event_->phase) {
        throw std::invalid_argument("Same-time event requires a strictly later phase");
    }
    if (at > horizon_) { return std::nullopt; }
    const auto sequence = checked_increment(last_sequence_);
    const EventKey key{at, phase, tie_key, sequence};
    // Event moves and comparator are nonthrowing; vector allocation happens
    // before heap reordering. Rejection/allocation failure consumes no sequence.
    queue_.push_back({key, std::move(action)});
    std::push_heap(queue_.begin(), queue_.end(), Later{});
    last_sequence_ = sequence;
    return key;
}

std::optional<EventKey> EventLoop::schedule_after(SimDuration delay, Phase phase, TieKey tie_key, Action action) {
    check_schedule(action);
    const auto at = deadline_within(now_, delay, horizon_);
    if (!at) { return std::nullopt; }
    return schedule(*at, phase, tie_key, std::move(action));
}

RunResult EventLoop::run() {
    if (state_ != RunState::ready) { throw std::logic_error("Event loop can run only once"); }
    state_ = RunState::running;
    try {
        while (!queue_.empty()) {
            if (max_events_ && executed_ >= *max_events_) { throw EventLimitExceeded{}; }
            const auto next_executed = checked_increment(executed_);
            std::pop_heap(queue_.begin(), queue_.end(), Later{});
            auto event = std::move(queue_.back());
            queue_.pop_back();
            now_ = event.key.at;
            last_event_ = event.key;
            event.action(*this);
            executed_ = next_executed;
        }
        now_ = horizon_;
        state_ = RunState::completed;
        return {now_, executed_};
    } catch (...) {
        state_ = RunState::failed;
        throw;
    }
}
}  // namespace telemetry::engine
