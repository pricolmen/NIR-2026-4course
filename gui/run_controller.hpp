#pragma once
#include "run_data.hpp"
#include "scenario_files.hpp"
#include <QFile>
#include <QObject>
#include <QProcess>
#include <QStringDecoder>

namespace telemetry::gui {
class RunController : public QObject {
    Q_OBJECT
public:
    explicit RunController(QObject* parent = nullptr);
    ~RunController() override;
    bool busy() const { return m_phase != Phase::Idle; }
    bool start(const RunRequest& request);
    bool startGenerated(const RunRequest& request,const QByteArray& config);
    bool saveCopy(const RunRequest& request,const ScenarioDocument& document,const QVector<FailureSpec>& failures,const QString& destination);
signals:
    void busyChanged(bool busy);
    void stageChanged(const QString& stage);
    void diagnostic(const QString& text);
    void succeeded(const telemetry::gui::RunResult& result);
    void failed(const QString& message);
    void scenarioGenerated(const QString& path);
    void copySaved(const QString& path);
private:
    enum class Phase { Idle, Preparing, Generating, Validating, Checking, Running, Loading, CopyValidating, CopyPublishing };
    Phase m_phase = Phase::Idle;
    QProcess* m_process;
    PreparedRun m_prepared;
    QFile m_log;
    QString m_details;
    QString m_command;
    QSharedPointer<CopyWorkspace> m_copy;
    QStringDecoder m_stdout{QStringDecoder::Utf8};
    QStringDecoder m_stderr{QStringDecoder::Utf8};
    void launch(const QStringList& arguments);
    void consume(bool errorStream);
    void finished(int exitCode, QProcess::ExitStatus status);
    void failure(const QString& message);
    void readyToValidate(const PreparedRun& prepared);
};
} // namespace telemetry::gui
