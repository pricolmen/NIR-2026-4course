#include "timeline_data.hpp"
#include "replay.hpp"

namespace telemetry::gui {
namespace {
void endAttempt(TimelineNode& node, const ReplayEvent& event, TimelinePoint at) {
    if (node.attempts.isEmpty()) { return; }
    auto& attempt = node.attempts.last();
    if (attempt.task == event.task && attempt.number == event.attempt && !attempt.end) {
        attempt.end = at;
    }
}
}

TimelineData buildTimeline(const ReplayData& replay) {
    TimelineData timeline;
    for (const auto& node : replay.nodes) {
        timeline.nodes[node].states.append(TimelineTransition{{}, "ALIVE"});
    }

    for (const auto& event : replay.events) {
        const TimelinePoint at{event.time, event.record};
        const auto mark = [&](TimelineEvent kind, const QString& detail = {}) {
            timeline.markers.append({at, kind, event.node, event.task, event.attempt, detail});
        };

        if (event.kind == "reconfiguration") {
            for (const auto& ref : event.revoked) {
                timeline.markers.append({at, TimelineEvent::Returned, ref.node, ref.task, ref.attempt, {}});
            }
            continue;
        }
        const auto node = timeline.nodes.find(event.node);
        if (node == timeline.nodes.end()) { continue; }

        if ((event.kind == "heartbeat_observed" || event.kind == "timeout_observed")
                && event.after != node->states.last().state) {
            node->states.append({at, event.after});
            mark(TimelineEvent::DetectorChanged, event.after);
        } else if (event.kind == "crash") {
            endAttempt(*node, event, at);
            mark(TimelineEvent::Crash);
        } else if (event.kind == "physical_started") {
            node->attempts.append({at, std::nullopt, event.task, event.attempt});
            mark(TimelineEvent::Started);
        } else if (event.kind == "physical_finished") {
            endAttempt(*node, event, at);
            mark(TimelineEvent::Finished);
        } else if (event.kind == "assignment_revoked") {
            mark(TimelineEvent::Revoked);
        } else if (event.kind == "task_assigned" && event.attempt != "1") {
            mark(TimelineEvent::Reassigned);
        } else if (event.kind == "result") {
            const auto kind = event.decision == "accepted" ? TimelineEvent::Accepted : TimelineEvent::Rejected;
            mark(kind, event.decision);
        }
    }
    return timeline;
}
} // namespace telemetry::gui
