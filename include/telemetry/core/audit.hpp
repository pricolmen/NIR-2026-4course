#pragma once

#include "telemetry/common/messages.hpp"
#include <variant>
#include <vector>

namespace telemetry::core {
enum class ResultDecision {
    accepted,
    unknown_task,
    already_completed,
    no_current_assignment,
    attempt_mismatch,
    node_mismatch
};

struct TaskArrivedEvent { SimTime at; TaskDescription task; };
struct TaskAssignedEvent { SimTime at; AssignmentCommand assignment; };
struct AssignmentRevokedEvent { SimTime at; ResultMessage assignment; };
struct ResultEvent { SimTime at; ResultMessage result; ResultDecision decision; };
// One coherent availability/revocation batch; no physical cancellation command.
struct ReconfigurationEvent {
    SimTime at;
    std::vector<NodeId> alive;
    std::vector<NodeId> newly_failed;
    std::vector<ResultMessage> revoked; // Original arrival, then task ID order.
};
using AuditEvent = std::variant<TaskArrivedEvent, TaskAssignedEvent, AssignmentRevokedEvent, ResultEvent, ReconfigurationEvent>;

class AuditSink {
public:
    virtual ~AuditSink() = default;
    // Called after a complete logical update. A sink error aborts the caller;
    // it does not roll back already committed state or external journal writes.
    virtual void on_event(const AuditEvent& event) = 0;
};
}  // namespace telemetry::core
