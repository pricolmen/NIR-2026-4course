#include "main_window.hpp"
#include "playback_widget.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTest>
#include <QTextStream>
#include <QUuid>

using namespace telemetry::gui;
namespace {
QString program,source,artifacts;
QByteArray read(const QString& path) { QFile file(path);if (!file.open(QIODevice::ReadOnly)) { qFatal("Cannot read acceptance input"); }return file.readAll(); }
void write(const QString& path,const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) { qFatal("Cannot write acceptance artifact"); }
}
QString criterion(const QString& name) {
    if (name == "baseline") { return "Узлы ALIVE; каждая задача имеет один принятый результат"; }
    if (name == "crash") { return "Физический отказ отделён от FAILED; задачи возвращены и имеют новую попытку"; }
    if (name == "blackout") { return "Исправный узел переходит в SUSPECT и возвращается в ALIVE"; }
    if (name == "long_blackout") { return "Ложный FAILED; результат отозванной попытки отклонён, другой принят"; }
    if (name == "no_alive") { return "Все узлы отказали; возвращённые задачи незавершены, Recovery с причиной"; }
    return "Неизвестный узел отклоняется validate до run; причина видна в окне";
}
}
class AcceptanceTests : public QObject {
    Q_OBJECT
    QJsonArray m_entries;
private slots:
    void initTestCase() { QVERIFY(QDir().mkpath(artifacts)); }
    void matrix_data() {
        QTest::addColumn<QString>("scenarioName");QTest::addColumn<QString>("detector");QTest::addColumn<QString>("scheduler");
        for (const auto* name : {"baseline","crash","blackout","long_blackout","no_alive","invalid"}) {
            for (const auto* detector : {"fixed","adaptive"}) { for (const auto* scheduler : {"rr","ll"}) {
                const auto row = QByteArray(name)+"-"+detector+"-"+scheduler;
                QTest::newRow(row.constData()) << QString(name) << QString(detector) << QString(scheduler);
            } }
        }
    }
    void matrix() {
        QFETCH(QString,scenarioName);QFETCH(QString,detector);QFETCH(QString,scheduler);
        const auto caseRoot = QDir(artifacts).filePath(scenarioName+"/"+detector+"_"+scheduler);
        QString scenario = QDir(source).filePath("examples/"+scenarioName+"/scenario.json");
        if (scenarioName == "invalid") {
            scenario = QDir(caseRoot).filePath("input/scenario.json");
            auto bytes = read(QDir(source).filePath("examples/crash/scenario.json"));bytes.replace("\"node_id\": 1","\"node_id\": 999");
            write(scenario,bytes);write(QDir(caseRoot).filePath("input/delivery.csv"),read(QDir(source).filePath("examples/crash/delivery.csv")));
        }
        const auto input = inspectInput(scenario);const auto jsonHash = sha256(scenario),traceHash = sha256(input.delivery);
        MainWindow window(program,QDir(caseRoot).filePath("runs"));window.show();QVERIFY(window.openScenario(scenario));
        auto* modes = window.findChild<QComboBox*>("detector");modes->setCurrentIndex(modes->findData(detector));
        auto* policies = window.findChild<QComboBox*>("scheduler");policies->setCurrentIndex(policies->findData(scheduler));
        QSignalSpy success(window.controller(),&RunController::succeeded),failure(window.controller(),&RunController::failed);
        QTest::mouseClick(window.findChild<QPushButton*>("startRun"),Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!window.controller()->busy(),20000);
        QJsonObject entry{{"case",scenarioName},{"detector",detector},{"scheduler",scheduler},{"scenario",scenario},
            {"scenario_sha256",QString::fromLatin1(jsonHash.toHex())},{"delivery_sha256",QString::fromLatin1(traceHash.toHex())},
            {"expected",criterion(scenarioName)},{"verification","Qt Test: real MainWindow/QProcess, saved journal projection and native metrics"}};
        if (scenarioName == "invalid") {
            QCOMPARE(success.size(),0);QCOMPARE(failure.size(),1);QCOMPARE(window.resultCount(),0);
            const auto error = window.findChild<QLabel*>("runError")->text();QVERIFY(error.contains("crashes") && error.contains("unknown"));
            const QDir runs(QDir(caseRoot).filePath("runs"));const auto folders = runs.entryList(QDir::Dirs | QDir::NoDotAndDotDot);QCOMPARE(folders.size(),1);
            const auto session = runs.filePath(folders.first());QVERIFY(!QFileInfo::exists(QDir(session).filePath("result")));
            const auto log = read(QDir(session).filePath("process.log"));QVERIFY(log.contains("\"validate\""));QVERIFY(!log.contains("\"run\""));
            entry["diagnostic"] = error;entry["actual"] = "Отклонён до расчёта";entry["output"] = session;
        } else {
            QCOMPARE(failure.size(),0);QCOMPARE(success.size(),1);const auto result = qvariant_cast<RunResult>(success.first().first());
            const auto data = result.playback;QVERIFY(data);auto* replay = window.playback()->controller();
            bool sawCrash = false,sawFailed = false,returned = false,reassigned = false,suspect = false,recovered = false,falseFailed = false;
            QSet<QString> accepted;QJsonArray transitions;
            for (const auto& event : data->events) {
                QVERIFY(replay->step());QCOMPARE(replay->state().record,event.record);
                if (event.kind == "crash") {
                    sawCrash = true;QVERIFY(replay->state().nodes[event.node].crashed);
                    transitions.append(QJsonObject{{"node",event.node},{"time_ticks",event.time},{"physical","crashed"}});
                }
                if (!event.after.isEmpty() && event.before != event.after) {
                    transitions.append(QJsonObject{{"node",event.node},{"time_ticks",event.time},{"detector",event.after}});
                    if (event.after == "SUSPECT") { suspect = true; }
                    if (event.after == "ALIVE" && suspect) { recovered = true; }
                    if (event.after == "FAILED") { sawFailed = true;if (!replay->state().nodes[event.node].crashed) { falseFailed = true; } }
                }
                if (!event.revoked.empty()) { returned = true; }
                if (event.kind == "task_assigned" && event.attempt.toULongLong() > 1) { reassigned = true; }
                if (event.kind == "result" && event.decision == "accepted") { QVERIFY(!accepted.contains(event.task));accepted.insert(event.task); }
            }
            QCOMPARE(replay->state().time,input.horizon);QVERIFY(replay->finished());
            QCOMPARE(accepted.size(),result.metrics.value("completed").toInteger());
            QCOMPARE(replay->state().queued.size(),result.metrics.value("queued").toInteger());
            QVERIFY(window.findChild<QPlainTextEdit*>("runSummary")->toPlainText().contains("Завершено задач: "+QString::number(accepted.size())));
            if (scenarioName == "baseline") {
                QVERIFY(!sawCrash && !sawFailed);QCOMPARE(accepted.size(),input.tasks);
                for (const auto& node : replay->state().nodes) { QVERIFY(!node.crashed);QCOMPARE(node.detector,QString("ALIVE")); }
            } else if (scenarioName == "crash") {
                QVERIFY(sawCrash && sawFailed && returned && reassigned);QCOMPARE(accepted.size(),input.tasks);
                QCOMPARE(result.metrics.value("mean_detection").toObject().value("value_ticks").toInteger(),17);
                if (scheduler == "rr") { QCOMPARE(result.metrics.value("mean_recovery").toObject().value("value_ticks").toInteger(),25); }
            } else if (scenarioName == "blackout") { QVERIFY(!sawCrash && suspect && recovered && !sawFailed); }
            else if (scenarioName == "long_blackout") {
                QVERIFY(!sawCrash && falseFailed && returned && reassigned);QVERIFY(result.metrics.value("false_positives").toInteger() > 0);
                bool attemptsDiffer = false;
                for (const auto& task : replay->state().tasks) {
                    QSet<QString> revoked,rejected,done;
                    for (const auto& row : task.history) {
                        if (row.event.contains("отозвана")) { revoked.insert(row.attempt); }
                        if (row.event.contains("отклонён")) { rejected.insert(row.attempt); }
                        if (row.event.contains("принят")) { done.insert(row.attempt); }
                    }
                    for (const auto& attempt : revoked) { if (rejected.contains(attempt) && !done.empty() && !done.contains(attempt)) { attemptsDiffer = true; } }
                }QVERIFY(attemptsDiffer);
            } else if (scenarioName == "no_alive") {
                QVERIFY(sawCrash && sawFailed && returned);QCOMPARE(accepted.size(),0);QVERIFY(result.metrics.value("unfinished").toInteger() > 0);
                for (const auto& node : replay->state().nodes) { QVERIFY(node.crashed);QCOMPARE(node.detector,QString("FAILED")); }
                auto* table = window.findChild<QTableWidget*>("nodeMetrics");
                for (int row = 0; row < table->rowCount(); ++row) { QVERIFY(table->item(row,3)->text().contains("recovery_incomplete")); }
            }
            entry["output"] = result.prepared.directory;entry["metrics"] = result.metrics;entry["transitions"] = transitions;
            const auto manifest = QJsonDocument::fromJson(read(QDir(result.prepared.directory).filePath("manifest.json"))).object();entry["configuration"] = manifest.value("parameters");
            entry["journal_sha256"] = QString::fromLatin1(sha256(QDir(result.prepared.directory).filePath("events.jsonl")).toHex());
            entry["actual"] = QString("Завершено %1, незавершено %2, FP %3").arg(result.metrics.value("completed").toInteger()).arg(result.metrics.value("unfinished").toInteger()).arg(result.metrics.value("false_positives").toInteger());
        }
        QCOMPARE(sha256(scenario),jsonHash);QCOMPARE(sha256(input.delivery),traceHash);
        if (detector == "fixed" && scheduler == "rr") { QTest::qWait(30);QVERIFY(window.grab().save(QDir(caseRoot).filePath("window.png"))); }
        entry["status"] = "passed";m_entries.append(entry);
    }
    void cleanupTestCase() {
        const QJsonObject report{{"schema_version",1},{"status",m_entries.size() == 24 ? "complete" : "failed"},
            {"note","Функциональная проверка GUI, не исследование превосходства алгоритмов"},{"cases_expected",24},{"cases_passed",m_entries.size()},{"entries",m_entries}};
        write(QDir(artifacts).filePath("acceptance.json"),QJsonDocument(report).toJson());
        QFile csv(QDir(artifacts).filePath("acceptance.csv"));QVERIFY(csv.open(QIODevice::WriteOnly));QTextStream stream(&csv);
        stream << "case,detector,scheduler,expected,actual,verification,output,status\n";
        for (const auto& value : m_entries) {
            const auto row = value.toObject();QStringList fields;
            for (const auto* key : {"case","detector","scheduler","expected","actual","verification","output","status"}) {
                auto text = row.value(key).toString();text.replace('"',"\"\"");fields.append('"'+text+'"');
            }stream << fields.join(',') << '\n';
        }QCOMPARE(m_entries.size(),24);
    }
};
int main(int argc,char** argv) {
    QApplication app(argc,argv);if (argc != 4) { return 2; }
    program = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();source = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
    artifacts = QDir(QString::fromLocal8Bit(argv[3])).absoluteFilePath("check-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    qInfo("Acceptance artifacts: %s",qPrintable(artifacts));AcceptanceTests tests;char* args[] = {argv[0],nullptr};return QTest::qExec(&tests,1,args);
}
#include "acceptance_tests.moc"
