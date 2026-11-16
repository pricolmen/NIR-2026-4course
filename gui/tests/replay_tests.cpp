#include "main_window.hpp"
#include "playback_widget.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QScrollArea>
#include <QTableWidget>
#include <QTest>
#include <QUuid>

using namespace telemetry::gui;
namespace { QString program,source,artifacts; }
class ReplayTests : public QObject {
    Q_OBJECT
    QMap<QString,QSharedPointer<const ReplayData>> m_data;
    QMap<QString,QString> m_directories;
private slots:
    void initTestCase() {
        QVERIFY(QDir().mkpath(artifacts));
        for (const auto* name : {"crash","baseline","blackout","long_blackout","no_alive"}) {
            const auto out = QDir(artifacts).filePath(name);QProcess process;
            process.start(program,{"run","--scenario",QDir(source).filePath(QString("examples/%1/scenario.json").arg(name)),
                "--detector","fixed","--scheduler","rr","--out",out});
            QVERIFY(process.waitForFinished(20000));QCOMPARE(process.exitCode(),0);
            m_data[name] = loadReplay(out);m_directories[name] = out;
        }
    }
    void crashOneRecordAtATime() {
        ReplayController replay;const auto data = m_data["crash"];replay.setData(data);
        QCOMPARE(replay.state().nodes["1"].detector,QString("ALIVE"));QVERIFY(!replay.state().nodes["1"].crashed);
        bool crash = false,suspect = false,failed = false,returned = false,one = false,three = false,last = false,equalTime = false;
        qint64 previousTime = -1;QSignalSpy applied(&replay,&ReplayController::recordApplied);
        for (const auto& event : data->events) {
            const auto before = applied.size();QVERIFY(replay.step());QCOMPARE(applied.size(),before+1);
            QCOMPARE(replay.state().record,event.record);QCOMPARE(replay.state().time,event.time);
            if (previousTime == event.time) { equalTime = true; }previousTime = event.time;
            if (event.kind == "crash" && event.node == "1") {
                QCOMPARE(event.time,3);QVERIFY(replay.state().nodes["1"].crashed);QCOMPARE(replay.state().nodes["1"].detector,QString("ALIVE"));crash = true;
            }
            if (event.node == "1" && event.after == "SUSPECT") { QCOMPARE(event.time,10);suspect = true; }
            if (event.node == "1" && event.after == "FAILED") { QCOMPARE(event.time,20);failed = true; }
            if (event.kind == "reconfiguration" && event.time == 20) { QVERIFY(replay.state().queued.contains("1"));QVERIFY(replay.state().queued.contains("3"));returned = true; }
            if (event.kind == "task_assigned" && event.attempt == "2") {
                QCOMPARE(event.node,QString("2"));QCOMPARE(event.time,20);if (event.task == "1") { one = true; }if (event.task == "3") { three = true; }
            }
            if (event.kind == "result" && event.task == "3" && event.decision == "accepted") { QCOMPARE(event.time,28);last = true; }
        }
        QVERIFY(crash && suspect && failed && returned && one && three && last && equalTime);
        QVERIFY(replay.finished());QVERIFY(!replay.step());QCOMPARE(replay.state().time,40);
        for (const auto& task : replay.state().tasks) { QCOMPARE(task.status,QString("Завершена")); }
        const auto node = data->metrics.first();QCOMPARE(node.detection,QString("17 мкс"));QCOMPARE(node.recovery,QString("25 мкс"));
    }
    void continuousMatchesStepsAndTakesEightSeconds() {
        const auto data = m_data["crash"];ReplayController steps,continuous;
        steps.setData(data);while (steps.step()) {}
        continuous.setData(data);QSignalSpy applied(&continuous,&ReplayController::recordApplied);
        QElapsedTimer clock;clock.start();continuous.play();
        QTRY_VERIFY_WITH_TIMEOUT(continuous.finished(),11000);
        const auto elapsed = clock.elapsed();QVERIFY2(elapsed >= 7800 && elapsed <= 10000,qPrintable(QString::number(elapsed)));
        QVERIFY(continuous.state() == steps.state());QCOMPARE(applied.size(),data->events.size());
        for (qsizetype i = 0; i < applied.size(); ++i) { QCOMPARE(applied[i].first().toLongLong(),data->events[i].record); }
        QFile file(QDir(artifacts).filePath("timing.json"));QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"nominal_ms",8000},{"actual_ms",elapsed},{"records",data->events.size()}}).toJson());
    }
    void timingPauseSpeedAndReset() {
        QCOMPARE(ReplayController::gapMilliseconds(10,40,1),2000.);QCOMPARE(ReplayController::gapMilliseconds(10,40,4),500.);
        QCOMPARE(ReplayController::gapMilliseconds(0,40,1),0.);
        QCOMPARE(ReplayController::gapMilliseconds(9007199254740993LL,9007199254740993LL,1),8000.);
        auto data = QSharedPointer<ReplayData>::create();data->horizon = 40;
        ReplayEvent a;a.record = 1;a.time = 0;a.kind = "run_started";
        ReplayEvent b;b.record = 2;b.time = 20;b.kind = "dispatch";
        ReplayEvent c;c.record = 3;c.time = 40;c.kind = "run_completed";data->events = {a,b,c};
        ReplayController replay;replay.setData(data);replay.play();QTest::qWait(100);replay.pause();
        const auto position = replay.state().record;QTest::qWait(150);QCOMPARE(replay.state().record,position);QVERIFY(!replay.playing());
        replay.setSpeed(8);replay.play();QTRY_VERIFY_WITH_TIMEOUT(replay.finished(),2000);
        replay.reset();QCOMPARE(replay.state().record,0);replay.play();QVERIFY(replay.step());QVERIFY(!replay.playing());QCOMPARE(replay.state().record,1);
        replay.play();replay.setSpeed(4);replay.reset();QTest::qWait(50);QCOMPARE(replay.state().record,0);
    }
    void physicalHealthAndDetectorAreIndependent() {
        ReplayController replay;replay.setData(m_data["blackout"]);bool suspect = false,recovered = false;
        while (replay.step()) {
            QVERIFY(!replay.state().nodes["1"].crashed);
            if (replay.state().nodes["1"].detector == "SUSPECT") { suspect = true; }
            if (suspect && replay.state().nodes["1"].detector == "ALIVE") { recovered = true; }
        }
        QVERIFY(suspect && recovered);
        replay.setData(m_data["long_blackout"]);while (replay.step()) {}
        QVERIFY(!replay.state().nodes["1"].crashed);QCOMPARE(replay.state().nodes["1"].detector,QString("FAILED"));
        bool both = false;
        for (const auto& task : replay.state().tasks) {
            bool revoked = false,rejected = false,accepted = false;
            for (const auto& row : task.history) {
                if (row.event.contains("отозвана") && row.attempt == "1") { revoked = true; }
                if (row.event.contains("отклонён") && row.attempt == "1") { rejected = true; }
                if (row.event.contains("принят") && row.attempt != "1") { accepted = true; }
            }
            if (revoked && rejected && accepted) { both = true; }
        }
        QVERIFY(both);
        PlaybackWidget widget;widget.setData(m_data["long_blackout"]);widget.show();while (widget.controller()->step()) {}
        const auto table = widget.findChild<QTableWidget*>("nodeStates");QCOMPARE(table->item(0,1)->text(),QString("Работоспособен"));
        QCOMPARE(table->item(0,2)->text(),QString("FAILED"));QTest::qWait(50);QVERIFY(widget.grab().save(QDir(artifacts).filePath("false-failed.png")));
    }
    void fullMetricsRemainIndependentAndReasonsAreVisible() {
        MainWindow window(program,QDir(artifacts).filePath("window-runs"));window.show();
        QVERIFY(window.openScenario(QDir(source).filePath("examples/crash/scenario.json")));
        QSignalSpy success(window.controller(),&RunController::succeeded);QTest::mouseClick(window.findChild<QPushButton*>("startRun"),Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(success.size(),1,20000);
        auto* playback = window.playback();auto* replay = playback->controller();const auto data = replay->data();
        auto* button = playback->findChild<QPushButton*>("step");
        while (!replay->finished()) {
            const auto& event = data->events[static_cast<qsizetype>(replay->state().record)];
            const auto target = event.kind == "task_assigned" && event.task == "1" && event.attempt == "2";
            QTest::mouseClick(button,Qt::LeftButton);if (target) { break; }
        }
        QCOMPARE(replay->state().time,20);const auto summary = window.findChild<QPlainTextEdit*>("runSummary")->toPlainText();
        QVERIFY(summary.contains("Завершено задач: 4"));QVERIFY(summary.contains("17 мкс"));QVERIFY(summary.contains("25 мкс"));
        auto* metrics = window.findChild<QTableWidget*>("nodeMetrics");QCOMPARE(metrics->item(0,2)->text(),QString("17 мкс"));QCOMPARE(metrics->item(0,3)->text(),QString("25 мкс"));
        QTest::qWait(100);
        const auto history = playback->findChild<QTableWidget*>("taskHistory");
        QVERIFY(history->viewport()->rect().contains(history->visualItemRect(history->item(history->rowCount()-1,4)).center()));
        const auto outer = window.findChild<QScrollArea*>("playbackScroll");
        const auto itemRect = history->visualItemRect(history->item(history->rowCount()-1,4));
        const auto lastPosition = history->viewport()->mapTo(outer->viewport(),itemRect.center());
        const QRect lastRect(history->viewport()->mapTo(outer->viewport(),itemRect.topLeft()),itemRect.size());
        qInfo("Native geometry: window=%dx%d outer=%dx%d history=%dx%d last=%d,%d",window.width(),window.height(),
            outer->viewport()->width(),outer->viewport()->height(),history->width(),history->height(),lastPosition.x(),lastPosition.y());
        QVERIFY(window.grab().save(QDir(artifacts).filePath("window-crash-20.png")));
        QVERIFY2(outer->viewport()->rect().contains(lastRect),"Latest task event must be fully visible without scrolling the outer panel");
        playback->findChild<QComboBox*>("selectedTask")->setCurrentText("3");
        const auto old = replay->state().record;QTest::mouseClick(playback->findChild<QPushButton*>("play"),Qt::LeftButton);
        QTest::mouseClick(playback->findChild<QPushButton*>("pause"),Qt::LeftButton);QVERIFY(!replay->playing());QVERIFY(replay->state().record >= old);
        playback->findChild<QComboBox*>("playbackSpeed")->setCurrentIndex(5);QTest::mouseClick(playback->findChild<QPushButton*>("play"),Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(replay->finished(),2000);
        QCOMPARE(window.findChild<QPlainTextEdit*>("runSummary")->toPlainText(),summary);
        QTest::mouseClick(playback->findChild<QPushButton*>("resetReplay"),Qt::LeftButton);QCOMPARE(replay->state().record,0);
        QTest::qWait(50);QVERIFY(window.grab().save(QDir(artifacts).filePath("window-reset-full-metrics.png")));
        // Null node metrics come from the native report, with the original reason.
        bool missing = false;for (const auto& node : m_data["no_alive"]->metrics) { if (node.recovery.contains("recovery_incomplete")) { missing = true; } }
        QVERIFY(missing);
    }
};
int main(int argc, char** argv) {
    QApplication app(argc,argv);if (argc != 4 && argc != 5) { return 2; }
    program = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();source = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
    artifacts = QDir(QString::fromLocal8Bit(argv[3])).absoluteFilePath("check-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    qInfo("Replay verification artifacts: %s",qPrintable(artifacts));
    ReplayTests tests;char* args[] = {argv[0],argc == 5 ? argv[4] : nullptr,nullptr};return QTest::qExec(&tests,argc == 5 ? 2 : 1,args);
}
#include "replay_tests.moc"
