#pragma once
#include "run_controller.hpp"
#include <QMainWindow>
#include <QVector>
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace telemetry::gui {
class PlaybackWidget;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QString simulator, QString resultsRoot, QWidget* parent = nullptr);
    bool openScenario(const QString& path);
    RunController* controller() { return &m_controller; }
    qsizetype resultCount() const { return m_results.size(); }
    PlaybackWidget* playback() { return m_playback; }
private:
    RunController m_controller;
    QLineEdit* m_scenario;
    QLineEdit* m_program;
    QLineEdit* m_output;
    QComboBox* m_detector;
    QComboBox* m_scheduler;
    QComboBox* m_history;
    QLabel* m_preview;
    QLabel* m_status;
    QLabel* m_error;
    QLabel* m_resultIdentity;
    QPlainTextEdit* m_summary;
    QPlainTextEdit* m_diagnostics;
    QProgressBar* m_progress;
    PlaybackWidget* m_playback;
    QTableWidget* m_nodeMetrics;
    QPushButton* m_start;
    QPushButton* m_editFailures;
    FormData m_lastForm = defaultForm();
    QVector<QWidget*> m_inputWidgets;
    QVector<RunResult> m_results;
    void updateResult(int index);
    bool showScenario(const QString& path);
};
} // namespace telemetry::gui
