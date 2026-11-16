#pragma once
#include "replay.hpp"
#include <QWidget>
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace telemetry::gui {
class PlaybackWidget : public QWidget {
    Q_OBJECT
public:
    explicit PlaybackWidget(QWidget* parent = nullptr);
    void setData(QSharedPointer<const ReplayData> data, const QString& identity = {});
    ReplayController* controller() { return &m_replay; }
private:
    ReplayController m_replay;
    QLabel* m_time;
    QLabel* m_identity;
    QLabel* m_position;
    QLabel* m_taskStatus;
    QTableWidget* m_nodes;
    QTableWidget* m_history;
    QComboBox* m_task;
    QPushButton* m_play;
    QPushButton* m_pause;
    QPushButton* m_step;
    QString m_visibleTask;
    qsizetype m_visibleRows = -1;
    void refresh();
};
} // namespace telemetry::gui
