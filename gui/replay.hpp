#pragma once
#include "timeline_data.hpp"
#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QSharedPointer>
#include <QStringList>
#include <QTimer>
#include <QVector>

namespace telemetry::gui {
struct AttemptRef { QString task,node,attempt; };
struct ReplayEvent {
    qint64 record = 0,time = 0;
    QString kind,node,task,attempt,before,after,decision;
    QVector<AttemptRef> revoked;
};
struct NodeMetric { QString node,classification,detection,recovery; };
struct ReplayData {
    qint64 horizon = 0;
    QStringList nodes,tasks;
    QVector<ReplayEvent> events;
    QVector<NodeMetric> metrics;
    TimelineData timeline;
};
struct NodeView {
    bool crashed = false;
    QString detector = "ALIVE";
    bool operator==(const NodeView&) const = default;
};
struct HistoryRow {
    qint64 record = 0,time = 0;
    QString attempt,node,event;
    bool operator==(const HistoryRow&) const = default;
};
struct TaskView {
    QString status = "Не поступила",attempt,node;
    QVector<HistoryRow> history;
    bool operator==(const TaskView&) const = default;
};
struct ReplayState {
    qint64 time = 0,record = 0;
    QMap<QString,NodeView> nodes;
    QMap<QString,TaskView> tasks;
    QSet<QString> queued;
    bool operator==(const ReplayState&) const = default;
};
QSharedPointer<const ReplayData> loadReplay(const QString& directory);
void projectEvent(ReplayState& state, const ReplayEvent& event);
class ReplayController : public QObject {
    Q_OBJECT
public:
    explicit ReplayController(QObject* parent = nullptr);
    void setData(QSharedPointer<const ReplayData> data);
    void reset();
    void play();
    void pause();
    bool step();
    void setSpeed(double speed);
    bool playing() const { return m_playing; }
    bool finished() const { return m_data && m_next == m_data->events.size(); }
    double speed() const { return m_speed; }
    const ReplayState& state() const { return m_state; }
    const QSharedPointer<const ReplayData>& data() const { return m_data; }
    // Pure timing conversion used by the wall-clock scheduler; ticks remain integers in the view.
    static double gapMilliseconds(qint64 delta, qint64 horizon, double speed);
signals:
    void changed();
    void playingChanged(bool playing);
    void recordApplied(qint64 record);
private:
    QSharedPointer<const ReplayData> m_data;
    ReplayState m_state;
    qsizetype m_next = 0;
    QTimer m_timer;
    QElapsedTimer m_clock;
    bool m_playing = false;
    double m_speed = 1;
    long double m_position = 0;
    void sync();
    void schedule();
    void tick();
    void apply();
};
} // namespace telemetry::gui
