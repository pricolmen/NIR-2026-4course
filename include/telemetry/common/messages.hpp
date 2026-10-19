#pragma once

#include "telemetry/common/ids.hpp"
#include "telemetry/common/time.hpp"

namespace telemetry {
struct TaskDescription {
    TaskId id;
    SimTime arrival;
    long double work;
    bool operator==(const TaskDescription&) const = default;
};

struct AssignmentCommand {
    TaskDescription task;
    NodeId node;
    AttemptNo attempt;
    bool operator==(const AssignmentCommand&) const = default;
};

struct ResultMessage {
    TaskId task;
    AttemptNo attempt;
    NodeId node;
    bool operator==(const ResultMessage&) const = default;
};
}  // namespace telemetry
