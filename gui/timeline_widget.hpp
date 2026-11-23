#pragma once

#include <QSharedPointer>
#include <QWidget>

class QLabel;
class QListWidget;

namespace telemetry::gui {
struct ReplayData;
class TimelineCanvas;

class TimelineWidget : public QWidget {
public:
    explicit TimelineWidget(QWidget* parent = nullptr);
    void setData(QSharedPointer<const ReplayData> replay);
    void setPosition(qint64 time, qint64 record);

private:
    QSharedPointer<const ReplayData> m_data;
    TimelineCanvas* m_canvas;
    QLabel* m_caption;
    QListWidget* m_events;
    qint64 m_time = -1;
    qint64 m_record = -1;
};
} // namespace telemetry::gui
