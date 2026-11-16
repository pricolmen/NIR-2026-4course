#include "run_controller.hpp"
#include "async.hpp"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>

namespace telemetry::gui {
RunController::RunController(QObject* parent) : QObject(parent),m_process(new QProcess(this)) {
    qRegisterMetaType<RunResult>();
    connect(m_process,&QProcess::readyReadStandardOutput,this,[this] { consume(false); });
    connect(m_process,&QProcess::readyReadStandardError,this,[this] { consume(true); });
    connect(m_process,&QProcess::finished,this,&RunController::finished);
    connect(m_process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
        if (busy() && error == QProcess::FailedToStart) {
            failure("Не удалось запустить telemetry_sim: " + m_process->errorString());
        }
    });
}
RunController::~RunController() {
    disconnect(m_process,nullptr,this,nullptr);
    if (m_process->state() != QProcess::NotRunning) { m_process->kill(); }
}
bool RunController::start(const RunRequest& request) {
    if (busy()) { return false; }
    m_copy.clear();
    m_phase = Phase::Preparing;
    emit busyChanged(true);
    emit stageChanged("Чтение сценария и трассы…");
    background<PreparedRun>(this,[request] { return prepareRun(request); },[this](const auto& outcome) {
        if (!outcome.value) { failure(outcome.error); return; }
        readyToValidate(*outcome.value);
    });
    return true;
}
void RunController::readyToValidate(const PreparedRun& prepared) {
    m_prepared = prepared;m_log.setFileName(QDir(m_prepared.session).filePath("process.log"));
    if (!m_log.open(QIODevice::WriteOnly)) { failure("Не удалось открыть журнал запуска: " + m_log.errorString());return; }
    m_phase = Phase::Validating;emit stageChanged("Проверка JSON и CSV…");launch({"validate","--scenario",m_prepared.input.scenario});
}
bool RunController::startGenerated(const RunRequest& request,const QByteArray& config) {
    if (busy()) { return false; }m_copy.clear();m_phase = Phase::Preparing;emit busyChanged(true);emit stageChanged("Подготовка конфигурации генератора…");
    background<PreparedRun>(this,[request,config] { return generationWorkspace(request,config); },[this](const auto& outcome) {
        if (!outcome.value) { failure(outcome.error);return; }m_prepared = *outcome.value;
        m_log.setFileName(QDir(m_prepared.session).filePath("process.log"));
        if (!m_log.open(QIODevice::WriteOnly)) { failure("Не удалось открыть журнал генерации");return; }
        m_phase = Phase::Generating;emit stageChanged("Генерация сценария и трассы…");
        launch({"generate","--config",QDir(m_prepared.session).filePath("generator_config.json"),"--out",QDir(m_prepared.session).filePath("generated")});
    });return true;
}
bool RunController::saveCopy(const RunRequest& request,const ScenarioDocument& document,const QVector<FailureSpec>& failures,const QString& destination) {
    if (busy()) { return false; }m_copy.clear();m_phase = Phase::Preparing;emit busyChanged(true);emit stageChanged("Подготовка копии сценария и CSV…");
    background<QSharedPointer<CopyWorkspace>>(this,[request,document,failures,destination] { return prepareCopy(request,document,failures,destination); },[this](const auto& outcome) {
        if (!outcome.value) { failure(outcome.error);return; }m_copy = *outcome.value;m_prepared = m_copy->process;
        m_log.setFileName(QDir(m_prepared.session).filePath("process.log"));
        if (!m_log.open(QIODevice::WriteOnly)) { failure("Не удалось открыть журнал проверки копии");return; }
        m_phase = Phase::CopyValidating;emit stageChanged("Проверка копии симулятором…");launch({"validate","--scenario",m_copy->json});
    });return true;
}
void RunController::launch(const QStringList& arguments) {
    m_command = arguments.value(0);
    m_details.clear();m_stdout.resetState();m_stderr.resetState();
    QJsonArray args;
    for (const auto& arg : arguments) { args.append(arg); }
    const auto line = QJsonDocument(QJsonObject{{"program",m_prepared.request.program},{"arguments",args}}).toJson(QJsonDocument::Compact);
    m_log.write("COMMAND " + line + '\n');m_log.flush();
    emit diagnostic(QString::fromUtf8(line));
    m_process->setWorkingDirectory(m_prepared.session);
    m_process->setProgram(m_prepared.request.program);
    m_process->setArguments(arguments); // Never a shell command or joined argument string.
    m_process->start();
}
void RunController::consume(bool errorStream) {
    const auto bytes = errorStream ? m_process->readAllStandardError() : m_process->readAllStandardOutput();
    if (bytes.isEmpty()) { return; }
    if (m_log.isOpen()) { m_log.write(bytes);m_log.flush(); }
    const QString text = errorStream ? m_stderr(bytes) : m_stdout(bytes);
    m_details = (m_details + text).right(1024*1024);
    emit diagnostic(text);
}
void RunController::finished(int exitCode, QProcess::ExitStatus status) {
    if (!busy()) { return; }
    consume(false);consume(true);
    if (status != QProcess::NormalExit || exitCode != 0) {
        const QString reason = status != QProcess::NormalExit ? "процесс аварийно завершён"
            : exitCode == 2 ? "ошибка аргументов команды" : "ошибка входных данных или выполнения";
        failure(QString("%1: %2 (код %3).\n%4").arg(m_command,reason).arg(exitCode).arg(m_details.trimmed()));
        return;
    }
    if (m_phase == Phase::Generating) {
        m_log.close();m_phase = Phase::Preparing;emit stageChanged("Чтение созданных входов…");const auto generation = m_prepared;
        background<PreparedRun>(this,[generation] { return generatedRun(generation); },[this](const auto& outcome) {
            if (!outcome.value) { failure(outcome.error);return; }
            emit scenarioGenerated(outcome.value->input.scenario);readyToValidate(*outcome.value);
        });
    } else if (m_phase == Phase::CopyValidating) {
        m_phase = Phase::CopyPublishing;emit stageChanged("Сохранение проверенной копии…");const auto copy = m_copy;
        background<QString>(this,[copy] { return publishCopy(copy); },[this](const auto& outcome) {
            if (!outcome.value) { failure(outcome.error);return; }
            m_log.close();m_copy.clear();m_phase = Phase::Idle;emit busyChanged(false);emit copySaved(*outcome.value);emit stageChanged("Копия сохранена — можно запустить расчёт");
        });
    } else if (m_phase == Phase::Validating) {
        m_phase = Phase::Checking;
        emit stageChanged("Проверка неизменности входов…");
        const auto prepared = m_prepared;
        background<bool>(this,[prepared] { ensureUnchanged(prepared);return true; },[this](const auto& outcome) {
            if (!outcome.value) { failure(outcome.error);return; }
            m_phase = Phase::Running;
            emit stageChanged("Выполняется расчёт…");
            launch({"run","--scenario",m_prepared.input.scenario,"--detector",m_prepared.request.detector,
                "--scheduler",m_prepared.request.scheduler,"--out",m_prepared.directory});
        });
    } else if (m_phase == Phase::Running) {
        m_phase = Phase::Loading;
        emit stageChanged("Проверка завершения и чтение результатов…");
        const auto prepared = m_prepared;
        background<RunResult>(this,[prepared] { return loadResult(prepared); },[this](const auto& outcome) {
            if (!outcome.value) { failure("Результат не загружен: " + outcome.error);return; }
            m_log.close();m_phase = Phase::Idle;
            emit busyChanged(false);
            emit stageChanged("Расчёт завершён — горизонт достигнут");
            emit succeeded(*outcome.value);
        });
    }
}
void RunController::failure(const QString& message) {
    if (m_log.isOpen()) { m_log.write(("GUI ERROR: " + message + '\n').toUtf8());m_log.close(); }
    m_copy.clear();m_phase = Phase::Idle;
    emit busyChanged(false);
    emit stageChanged("Ошибка — исправьте входы и повторите запуск");
    emit diagnostic(message);
    emit failed(message);
}
} // namespace telemetry::gui
