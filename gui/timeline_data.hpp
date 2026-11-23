#pragma once

#include <QMap>
#include <QString>
#include <QVector>
#include <optional>

namespace telemetry::gui {
struct ReplayData;

struct TimelinePoint {
    qint64 time = 0;
    qint64 record = 0;
};

struct TimelineTransition {
    TimelinePoint at;
    QString state;
};

struct TimelineAttempt {
    TimelinePoint start;
    std::optional<TimelinePoint> end;
    QString task, number;
};

enum class TimelineEvent {
    DetectorChanged, Crash, Started, Finished, Revoked, Returned, Reassigned, Accepted, Rejected
};

struct TimelineMarker {
    TimelinePoint at;
    TimelineEvent kind;
    QString node, task, attempt, detail;
};

struct TimelineNode {
    QVector<TimelineTransition> states;
    QVector<TimelineAttempt> attempts;
};

struct TimelineData {
    QMap<QString,TimelineNode> nodes;
    QVector<TimelineMarker> markers;
};

TimelineData buildTimeline(const ReplayData& replay);
} // namespace telemetry::gui
