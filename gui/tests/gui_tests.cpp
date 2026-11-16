#include "main_window.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <limits>

using namespace telemetry::gui;
namespace {
QString program,fake,source,artifacts;
QByteArray read(const QString& path) { QFile f(path);if (!f.open(QIODevice::ReadOnly)) { qFatal("Cannot read fixture"); }return f.readAll(); }
void write(const QString& path, const QByteArray& value) {
    QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(value) != value.size()) { qFatal("Cannot write fixture"); }
}
void saveObject(const QString& path, const QJsonObject& value) { write(path,QJsonDocument(value).toJson()); }
QString crashCopy(const QString& root, bool absoluteTrace = false) {
    const auto csv = QDir(root).filePath("трассы с пробелами/канал.csv");write(csv,read(QDir(source).filePath("examples/crash/delivery.csv")));
    const auto path = QDir(root).filePath("входы с пробелами/сценарий.json");
    auto data = read(QDir(source).filePath("examples/crash/scenario.json"));
    const auto trace = absoluteTrace ? csv : "../трассы с пробелами/канал.csv";
    data.replace("\"delivery.csv\"",QJsonDocument(QJsonArray{trace}).toJson(QJsonDocument::Compact).mid(1).chopped(1));
    write(path,data);return path;
}
RunResult waitSuccess(RunController& controller, const RunRequest& request) {
    QSignalSpy success(&controller,&RunController::succeeded),failed(&controller,&RunController::failed);
    if (!controller.start(request)) { qFatal("Start rejected"); }
    QElapsedTimer timer;timer.start();
    while (controller.busy() && timer.elapsed() < 20000) { QTest::qWait(5); }
    if (success.size() != 1 || !failed.empty()) { qFatal("Run did not succeed: %s",failed.empty() ? "timeout" : qPrintable(failed.first().first().toString())); }
    return qvariant_cast<RunResult>(success.first().first());
}
bool chooseScenario(MainWindow& window, const QString& scenario) {
    bool chosen = false;
    QTimer::singleShot(100,&window,[&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QFileDialog*>(widget)) {
                dialog->setDirectory(QFileInfo(scenario).absolutePath());
                chosen = true;
                QTimer::singleShot(300,dialog,[dialog,scenario] {
                    if (auto* edit = dialog->findChild<QLineEdit*>("fileNameEdit")) {
                        edit->setText(QFileInfo(scenario).fileName());
                        QTest::keyClick(edit,Qt::Key_Return);
                    }
                });
                QTimer::singleShot(3000,dialog,[dialog] { dialog->reject(); });
                return;
            }
        }
    });
    QTest::mouseClick(window.findChild<QPushButton*>("openScenario"),Qt::LeftButton);
    const auto selected = window.findChild<QLineEdit*>("scenarioPath")->text();
    if (selected.isEmpty()) { qWarning("Dialog did not select file; found=%d",chosen); }
    return chosen && !selected.isEmpty() && QFileInfo(selected).canonicalFilePath() == QFileInfo(scenario).canonicalFilePath();
}
}
class GuiTests : public QObject {
    Q_OBJECT
    QString m_fixture;
private slots:
    void initTestCase() {
        QVERIFY(QDir().mkpath(artifacts));
        m_fixture = QDir(artifacts).filePath("native-crash-fixture");
        QProcess process;
        process.start(program,{"run","--scenario",QDir(source).filePath("examples/crash/scenario.json"),
            "--detector","fixed","--scheduler","rr","--out",m_fixture});
        QVERIFY(process.waitForFinished(20000));QCOMPARE(process.exitCode(),0);
        qputenv("TELEMETRY_GUI_FIXTURE_RUN",m_fixture.toUtf8());
    }
    void cleanup() { qunsetenv("TELEMETRY_GUI_FAKE_MODE"); }
    void realCombinations_data() {
        QTest::addColumn<QString>("detector");QTest::addColumn<QString>("scheduler");QTest::addColumn<bool>("absoluteTrace");
        QTest::newRow("fixed-rr-relative") << QString("fixed") << QString("rr") << false;
        QTest::newRow("fixed-ll-absolute") << QString("fixed") << QString("ll") << true;
        QTest::newRow("adaptive-rr-relative") << QString("adaptive") << QString("rr") << false;
        QTest::newRow("adaptive-ll-absolute") << QString("adaptive") << QString("ll") << true;
    }
    void realCombinations() {
        QFETCH(QString,detector);QFETCH(QString,scheduler);QFETCH(bool,absoluteTrace);
        const auto root = QDir(artifacts).filePath("комбинации/"+detector+"_"+scheduler);
        const auto scenario = crashCopy(root,absoluteTrace);
        const auto executable = QDir(root).filePath("программа с пробелами/telemetry sim");QDir().mkpath(QFileInfo(executable).absolutePath());
        QVERIFY(QFile::copy(program,executable));
        const auto beforeJson = sha256(scenario);const auto beforeTrace = sha256(inspectInput(scenario).delivery);
        RunController controller;
        const auto result = waitSuccess(controller,{scenario,executable,QDir(root).filePath("выходные данные"),detector,scheduler});
        QCOMPARE(result.metrics.value("completed").toInteger(),4);QCOMPARE(result.metrics.value("unfinished").toInteger(),0);
        QCOMPARE(result.prepared.input.horizon,40);QVERIFY(result.records > 0);
        QCOMPARE(sha256(scenario),beforeJson);QCOMPARE(sha256(result.prepared.input.delivery),beforeTrace);
        const auto again = waitSuccess(controller,{scenario,executable,QDir(root).filePath("выходные данные"),detector,scheduler});
        QVERIFY(again.prepared.directory != result.prepared.directory);
        QCOMPARE(sha256(QDir(again.prepared.directory).filePath("events.jsonl")),sha256(QDir(result.prepared.directory).filePath("events.jsonl")));
        if (detector == "fixed" && scheduler == "rr") {
            QCOMPARE(result.metrics.value("mean_detection").toObject().value("value_ticks").toDouble(),17.0);
            QCOMPARE(result.metrics.value("mean_recovery").toObject().value("value_ticks").toDouble(),25.0);
        }
    }
    void unknownNodeRejectedBeforeRun() {
        const auto root = QDir(artifacts).filePath("unknown-node");const auto scenario = crashCopy(root);
        auto data = QJsonDocument::fromJson(read(scenario)).object();data["crashes"] = QJsonArray{QJsonObject{{"node_id",999},{"at_ticks",3}}};saveObject(scenario,data);
        RunController controller;QSignalSpy failure(&controller,&RunController::failed),success(&controller,&RunController::succeeded);
        QVERIFY(controller.start({scenario,program,QDir(root).filePath("runs")}));
        QTRY_COMPARE_WITH_TIMEOUT(failure.size(),1,20000);QVERIFY(success.empty());QVERIFY(!controller.busy());
        QVERIFY(failure.first().first().toString().contains("crashes"));
        const auto sessions = QDir(QDir(root).filePath("runs")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);QCOMPARE(sessions.size(),1);
        const auto session = QDir(root).filePath("runs/"+sessions.first());
        const auto log = read(QDir(session).filePath("process.log"));QVERIFY(log.contains("validate"));QVERIFY(!log.contains("\"run\""));
        QVERIFY(!QFileInfo::exists(QDir(session).filePath("result")));
    }
    void faults_data() {
        QTest::addColumn<QByteArray>("mode");QTest::addColumn<QString>("expected");
        const QMap<QByteArray,QString> faults{{"exit1","код 1"},{"exit2","аргументов"},{"crash","аварийно"},
            {"no-files","manifest.json"},{"incomplete","status=complete"},{"no-marker","завершающей записи"},
            {"wrong-horizon","порядок"},{"after-marker","После run_completed"},{"wrong-input","Снимки"},
            {"wrong-variant","другой пары"},{"bad-metrics","ошибка чтения JSON"},{"mutate-input","изменились"},
            {"journal-checksum","Контрольная сумма"}};
        for (auto it = faults.cbegin(); it != faults.cend(); ++it) { QTest::newRow(it.key().constData()) << it.key() << it.value(); }
    }
    void faults() {
        QFETCH(QByteArray,mode);QFETCH(QString,expected);qputenv("TELEMETRY_GUI_FAKE_MODE",mode);
        QTemporaryDir temp;const auto scenario = crashCopy(temp.path());
        RunController controller;QSignalSpy failure(&controller,&RunController::failed),success(&controller,&RunController::succeeded);
        QVERIFY(controller.start({scenario,fake,QDir(temp.path()).filePath("runs")}));
        QTRY_COMPARE_WITH_TIMEOUT(failure.size(),1,20000);QVERIFY(success.empty());QVERIFY(!controller.busy());
        QVERIFY(!failure.first().first().toString().isEmpty());
        QVERIFY2(failure.first().first().toString().contains(expected),qPrintable(failure.first().first().toString()));
        if (mode == "exit2") { QVERIFY(failure.first().first().toString().contains("аргументов")); }
        if (mode == "mutate-input") {
            const auto dirs = QDir(QDir(temp.path()).filePath("runs")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            QVERIFY(!read(QDir(temp.path()).filePath("runs/"+dirs.first()+"/process.log")).contains("\"run\""));
        }
    }
    void missingProgramAndMissingTrace() {
        QTemporaryDir temp;const auto scenario = crashCopy(temp.path());
        RunController controller;QSignalSpy failure(&controller,&RunController::failed);
        QVERIFY(controller.start({scenario,QDir(temp.path()).filePath("not-installed"),temp.path()}));
        QTRY_COMPARE(failure.size(),1);failure.clear();
        QVERIFY(QFile::remove(inspectInput(scenario).delivery));
        QVERIFY(controller.start({scenario,program,temp.path()}));QTRY_COMPARE(failure.size(),1);
        QVERIFY(failure.first().first().toString().contains("трассу"));
    }
    void processFailedToStart() {
        QTemporaryDir temp;const auto scenario = crashCopy(temp.path());
        const auto bad = QDir(temp.path()).filePath("invalid executable");write(bad,"not an executable format");
        QVERIFY(QFile::setPermissions(bad,QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        RunController controller;QSignalSpy failure(&controller,&RunController::failed);
        QVERIFY(controller.start({scenario,bad,QDir(temp.path()).filePath("runs")}));QTRY_COMPARE_WITH_TIMEOUT(failure.size(),1,20000);
        QVERIFY(failure.first().first().toString().contains("Не удалось запустить"));
    }
    void responsiveDuringBothProcesses() {
        qputenv("TELEMETRY_GUI_FAKE_MODE","slow");QTemporaryDir temp;const auto scenario = crashCopy(temp.path());
        MainWindow window(fake,QDir(temp.path()).filePath("runs"));QVERIFY(window.openScenario(scenario));window.show();
        auto* button = window.findChild<QPushButton*>("startRun");QVERIFY(button);
        QSignalSpy success(window.controller(),&RunController::succeeded);
        int ticks = 0;QTimer heartbeat;heartbeat.setInterval(10);connect(&heartbeat,&QTimer::timeout,this,[&ticks] { ++ticks; });heartbeat.start();
        QTest::mouseClick(button,Qt::LeftButton);QVERIFY(window.controller()->busy());QVERIFY(!button->isEnabled());
        QVERIFY(!window.controller()->start({scenario,program,temp.path()}));QVERIFY(!window.openScenario(scenario));
        QTRY_COMPARE_WITH_TIMEOUT(success.size(),1,20000);QVERIFY(ticks >= 20);QVERIFY(button->isEnabled());
    }
    void unfinishedIsSuccessful() {
        QTemporaryDir temp;const auto scenario = crashCopy(temp.path());auto data = QJsonDocument::fromJson(read(scenario)).object();
        data["crashes"] = QJsonArray{QJsonObject{{"node_id",1},{"at_ticks",0}},QJsonObject{{"node_id",2},{"at_ticks",0}}};saveObject(scenario,data);
        RunController controller;const auto result = waitSuccess(controller,{scenario,program,QDir(temp.path()).filePath("runs")});
        QCOMPARE(result.metrics.value("completed").toInteger(),0);QCOMPARE(result.metrics.value("unfinished").toInteger(),4);
        QVERIFY(durationText(result.metrics.value("mean_recovery").toObject()).contains("empty_sample"));
    }
    void exactIntegerHorizonAndInputBytes() {
        const auto root = QDir(artifacts).filePath("large-integers");const auto scenario = QDir(root).filePath("scenario.json");
        const QByteArray json = R"({"schema_version":1,"tick_ns":1000,"horizon_ticks":9007199254740993,"heartbeat_period_ticks":9007199254740993,"tau0_ticks":9007199254740993,"kappa":2,"adaptive":{"window":3,"min_intervals":2,"c":2},"nodes":[{"id":18446744073709551615,"performance_work_per_second":1000000}],"tasks":[],"crashes":[],"blackouts":[],"measurement_windows":[],"delivery_trace":"delivery.csv"})";
        write(scenario,json);write(QDir(root).filePath("delivery.csv"),"node_id,heartbeat_seq,delay_ticks,lost\n18446744073709551615,1,0,0\n");
        RunController controller;const auto result = waitSuccess(controller,{scenario,program,QDir(root).filePath("runs")});
        QCOMPARE(result.prepared.input.horizon,qint64(9007199254740993LL));
        QVERIFY(result.playback);QCOMPARE(result.playback->nodes.first(),QString("18446744073709551615"));
        QCOMPARE(read(QDir(result.prepared.directory).filePath("input/scenario.json")),json);
    }
    void notDetectedIsTranslated() {
        const auto root = QDir(artifacts).filePath("late-crash");const auto scenario = crashCopy(root);
        auto json = QJsonDocument::fromJson(read(scenario)).object();
        json["crashes"] = QJsonArray{QJsonObject{{"node_id",1},{"at_ticks",39}}};saveObject(scenario,json);
        MainWindow window(program,QDir(root).filePath("runs"));window.show();QVERIFY(window.openScenario(scenario));
        QSignalSpy success(window.controller(),&RunController::succeeded),failure(window.controller(),&RunController::failed);
        QTest::mouseClick(window.findChild<QPushButton*>("startRun"),Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(success.size(),1,20000);QVERIFY(failure.empty());
        const auto result = qvariant_cast<RunResult>(success.first().first());
        const auto native = result.metrics.value("nodes").toArray().first().toObject();
        QCOMPARE(native.value("classification").toString(),QString("not_detected"));
        auto* table = window.findChild<QTableWidget*>("nodeMetrics");QCOMPARE(table->item(0,0)->text(),QString("1"));
        QCOMPARE(table->item(0,1)->text(),QString("Отказ не обнаружен"));
        QVERIFY(table->item(0,2)->text().contains("отказ не обнаружен до горизонта"));
        QVERIFY(table->item(0,3)->text().contains("отказ не обнаружен до горизонта"));
        QTest::qWait(30);QVERIFY(window.grab().save(QDir(root).filePath("window-not-detected.png")));
    }
    void scenarioNumberRangeMatchesCli_data() {
        QTest::addColumn<QByteArray>("performance");QTest::addColumn<bool>("supported");
        QTest::newRow("within-double") << QByteArray("1e300") << true;
        QTest::newRow("beyond-double") << QByteArray("1e400") << (std::numeric_limits<long double>::max_exponent10 >= 400);
        QTest::newRow("beyond-long-double") << QByteArray("1e10000") << false;
    }
    void scenarioNumberRangeMatchesCli() {
        QFETCH(QByteArray,performance);QFETCH(bool,supported);
        const auto root = QDir(artifacts).filePath("number-range/"+QString::fromLatin1(performance));const auto scenario = crashCopy(root);
        auto json = read(scenario);QVERIFY(json.contains("1000000"));json.replace("1000000",performance);write(scenario,json);
        QProcess cli;cli.start(program,{"validate","--scenario",scenario});QVERIFY(cli.waitForFinished(20000));
        QCOMPARE(cli.exitStatus(),QProcess::NormalExit);QCOMPARE(cli.exitCode(),supported ? 0 : 1);
        write(QDir(root).filePath("validate.log"),cli.readAllStandardOutput()+cli.readAllStandardError());
        qInfo("performance=%s long-double max_exponent10=%d supported=%d",performance.constData(),std::numeric_limits<long double>::max_exponent10,supported);
        MainWindow window(program,QDir(root).filePath("runs"));window.show();QCOMPARE(window.openScenario(scenario),supported);
        if (!supported) {
            QVERIFY(window.findChild<QLabel*>("runError")->text().contains("ошибка чтения JSON"));return;
        }
        const auto info = inspectInput(scenario);QCOMPARE(info.horizon,40);QCOMPARE(info.nodes,2);QCOMPARE(info.tasks,4);
        QSignalSpy success(window.controller(),&RunController::succeeded),failure(window.controller(),&RunController::failed);
        QTest::mouseClick(window.findChild<QPushButton*>("startRun"),Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(success.size(),1,20000);QVERIFY(failure.empty());
        const auto result = qvariant_cast<RunResult>(success.first().first());
        QCOMPARE(result.prepared.input.nodes,2);QCOMPARE(result.prepared.input.tasks,4);
        QCOMPARE(result.metrics.value("completed").toInteger(),4);
        QCOMPARE(read(QDir(result.prepared.directory).filePath("input/scenario.json")),json);
        QCOMPARE(read(scenario),json);
    }
    void windowWorkflowAndPreviousBinding() {
        const auto root = QDir(artifacts).filePath("window-workflow");const auto scenario = crashCopy(root);
        MainWindow window(program,QDir(root).filePath("runs"));window.show();QVERIFY(chooseScenario(window,scenario));
        auto* button = window.findChild<QPushButton*>("startRun");
        auto* summary = window.findChild<QPlainTextEdit*>("runSummary");auto* identity = window.findChild<QLabel*>("resultIdentity");
        QSignalSpy success(window.controller(),&RunController::succeeded),failure(window.controller(),&RunController::failed);
        QTest::mouseClick(button,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(success.size(),1,20000);
        QCOMPARE(window.resultCount(),1);QVERIFY(summary->toPlainText().contains("Завершено задач: 4"));
        QVERIFY(summary->toPlainText().contains("17 мкс"));QVERIFY(summary->toPlainText().contains("25 мкс"));
        QTest::qWait(80);QVERIFY(window.grab().save(QDir(artifacts).filePath("window-success.png")));
        const auto previous = identity->text();const auto previousSummary = summary->toPlainText();
        const auto bad = QDir(root).filePath("other/scenario.json");write(bad,read(scenario));write(QDir(root).filePath("трассы с пробелами/канал.csv"),read(inspectInput(scenario).delivery));
        auto data = QJsonDocument::fromJson(read(bad)).object();
        data["delivery_trace"] = inspectInput(scenario).delivery;
        data["crashes"] = QJsonArray{QJsonObject{{"node_id",999},{"at_ticks",3}}};saveObject(bad,data);
        QVERIFY(chooseScenario(window,bad));window.findChild<QComboBox*>("detector")->setCurrentIndex(1);
        window.findChild<QComboBox*>("scheduler")->setCurrentIndex(1);
        QCOMPARE(identity->text(),previous);QTest::mouseClick(button,Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(failure.size(),1,20000);QCOMPARE(window.resultCount(),1);
        QCOMPARE(identity->text(),previous);QCOMPARE(summary->toPlainText(),previousSummary);
        const auto error = window.findChild<QLabel*>("runError")->text();QVERIFY(error.contains("crashes"));
        QTest::qWait(80);QVERIFY(window.grab().save(QDir(artifacts).filePath("window-invalid.png")));
        data["crashes"] = QJsonArray{QJsonObject{{"node_id",1},{"at_ticks",3}}};saveObject(bad,data);
        // The input was fixed in place, without choosing a new file in the GUI.
        QTest::mouseClick(button,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(success.size(),2,20000);QCOMPARE(window.resultCount(),2);
        auto* history = window.findChild<QComboBox*>("runHistory");QCOMPARE(history->count(),2);
        history->setCurrentIndex(0);QCOMPARE(identity->text(),previous);
    }
};
int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv);
    if (argc != 5) { return 2; }
    program = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();fake = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
    source = QFileInfo(QString::fromLocal8Bit(argv[3])).absoluteFilePath();
    artifacts = QDir(QFileInfo(QString::fromLocal8Bit(argv[4])).absoluteFilePath()).filePath("check-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    qInfo("GUI verification artifacts: %s",qPrintable(artifacts));
    GuiTests tests;char* args[] = {argv[0],nullptr};return QTest::qExec(&tests,1,args);
}
#include "gui_tests.moc"
