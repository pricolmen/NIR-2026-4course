#include "main_window.hpp"
#include "scenario_dialogs.hpp"
#include "playback_widget.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QScrollArea>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <nlohmann/json.hpp>

using namespace telemetry::gui;
namespace {
QString program,fake,source,artifacts;
QByteArray read(const QString& path) { QFile file(path);if (!file.open(QIODevice::ReadOnly)) { qFatal("Cannot read fixture"); }return file.readAll(); }
void write(const QString& path,const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) { qFatal("Cannot write fixture"); }
}
QString fixture(const QString& root) {
    const auto path = QDir(root).filePath("входы с пробелами/сценарий.json");
    write(path,read(QDir(source).filePath("examples/crash/scenario.json")));
    write(QDir(QFileInfo(path).absolutePath()).filePath("delivery.csv"),read(QDir(source).filePath("examples/crash/delivery.csv")));return path;
}
template<class Work> bool rejected(Work work) { try { work();return false; } catch (const std::exception&) { return true; } }
FormData parameters() {
    auto data = defaultForm();data.fields["horizon"] = "100";data.fields["arrivalPeriod"] = "1";data.fields["workMin"] = "4";data.fields["workMax"] = "4";
    data.fields["seed"] = "18446744073709551615";data.failures = {{"1","3"}};return data;
}
RunRequest request(const QString& root,const QString& scenario = {}) { return {scenario,program,QDir(root).filePath("данные прогонов")}; }
bool idle(RunController& controller) {
    QElapsedTimer timer;timer.start();while (controller.busy() && timer.elapsed() < 20000) { QTest::qWait(5); }return !controller.busy();
}
}
class EditorTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(QDir().mkpath(artifacts)); }
    void cleanup() { qunsetenv("TELEMETRY_GUI_FAKE_MODE"); }
    void formRulesAndBoundaries() {
        auto good = parameters();QVERIFY(generatorConfig(good).contains("18446744073709551615"));
        good.failures = {};QVERIFY(!generatorConfig(good).isEmpty());
        good.failures = {{"1","0"},{"2","100"}};QVERIFY(!generatorConfig(good).isEmpty());
        good.failures = {{"1","50"},{"2","50"}};QVERIFY(!generatorConfig(good).isEmpty());
        auto bad = good;bad.failures = {{"1","0"},{"1","1"}};QVERIFY(rejected([&] { generatorConfig(bad); }));
        bad = good;bad.failures = {{"3","0"}};QVERIFY(rejected([&] { generatorConfig(bad); }));
        bad = good;bad.failures = {{"1","101"}};QVERIFY(rejected([&] { generatorConfig(bad); }));
        bad = good;bad.failures = {{"1","-1"}};QVERIFY(rejected([&] { generatorConfig(bad); }));
        bad = good;bad.nodes.clear();QVERIFY(rejected([&] { generatorConfig(bad); }));
        bad = good;bad.nodes[1][0] = "1";QVERIFY(rejected([&] { generatorConfig(bad); }));
        bad = good;bad.nodes[0][1] = "0";QVERIFY(rejected([&] { generatorConfig(bad); }));
        for (const auto& field : {QString("heartbeat"),QString("timeout"),QString("arrivalPeriod")}) {
            bad = good;bad.fields[field] = "0";QVERIFY(rejected([&] { generatorConfig(bad); }));
        }
        bad = good;bad.fields["seed"] = "18446744073709551616";QVERIFY(rejected([&] { generatorConfig(bad); }));
        bad = good;bad.fields["loss"] = "1.1";QVERIFY(rejected([&] { generatorConfig(bad); }));
        bad = good;bad.blackouts = {{"3","0","10"}};QVERIFY(rejected([&] { generatorConfig(bad); }));
        bad = good;bad.blackouts = {{"1","5","5"}};QVERIFY(rejected([&] { generatorConfig(bad); }));
        good.blackouts = {{"1","90","110"}};QVERIFY(!generatorConfig(good).isEmpty()); // Native permits an end beyond horizon.
    }
    void generationThenReopen() {
        const auto root = QDir(artifacts).filePath("generation");RunController controller;QSignalSpy success(&controller,&RunController::succeeded),failure(&controller,&RunController::failed),generated(&controller,&RunController::scenarioGenerated),diagnostic(&controller,&RunController::diagnostic);
        const auto config = generatorConfig(parameters());QVERIFY(controller.startGenerated(request(root),config));QVERIFY(idle(controller));
        QCOMPARE(failure.size(),0);QCOMPARE(success.size(),1);QCOMPARE(generated.size(),1);
        const auto result = qvariant_cast<RunResult>(success.first().first());QCOMPARE(result.metrics.value("completed").toInteger(),4);
        const auto scenario = generated.first().first().toString();QVERIFY(QFileInfo::exists(scenario));
        const auto generationDir = QFileInfo(scenario).absolutePath();QCOMPARE(read(QDir(generationDir).filePath("generator_config.json")),config);
        const auto manifest = nlohmann::json::parse(read(QDir(generationDir).filePath("generation_manifest.json")).toStdString());
        QCOMPARE(manifest.at("seed").get<quint64>(),std::numeric_limits<quint64>::max());
        QString commands;for (const auto& row : diagnostic) { commands += row.first().toString(); }
        const auto gen = commands.indexOf("\"arguments\":[\"generate\"");const auto validate = commands.indexOf("\"arguments\":[\"validate\"");const auto run = commands.indexOf("\"arguments\":[\"run\"");
        QVERIFY(gen >= 0 && validate > gen && run > validate);
        const auto before = sha256(scenario),trace = sha256(inspectInput(scenario).delivery);
        QVERIFY(controller.start(request(root,scenario)));QVERIFY(idle(controller));QCOMPARE(success.size(),2);
        const auto again = qvariant_cast<RunResult>(success.last().first());
        QCOMPARE(sha256(scenario),before);QCOMPARE(sha256(again.prepared.input.delivery),trace);
        QCOMPARE(sha256(QDir(result.prepared.directory).filePath("events.jsonl")),sha256(QDir(again.prepared.directory).filePath("events.jsonl")));
    }
    void emptyAndSimultaneousGeneratedFailures_data() {
        QTest::addColumn<int>("kind");QTest::newRow("empty") << 0;QTest::newRow("simultaneous") << 1;QTest::newRow("horizon") << 2;
    }
    void emptyAndSimultaneousGeneratedFailures() {
        QFETCH(int,kind);auto data = parameters();data.failures = kind == 0 ? InputRows{} : kind == 1 ? InputRows{{"1","3"},{"2","3"}} : InputRows{{"1","100"},{"2","100"}};
        RunController controller;QSignalSpy success(&controller,&RunController::succeeded),failure(&controller,&RunController::failed);
        QVERIFY(controller.startGenerated(request(QDir(artifacts).filePath("failure-case-"+QString::number(kind))),generatorConfig(data)));QVERIFY(idle(controller));
        QCOMPARE(failure.size(),0);QCOMPARE(success.size(),1);const auto result = qvariant_cast<RunResult>(success.first().first());
        QCOMPARE(result.metrics.value("crashes").toInteger(),kind == 0 ? 0 : 2);
        if (kind == 1) { QVERIFY(result.metrics.value("unfinished").toInteger() > 0); }
    }
    void nativeGenerationValidationAndFalseSuccess() {
        auto data = parameters();data.fields["taskCount"] = "200";RunController controller;QSignalSpy failed(&controller,&RunController::failed),generated(&controller,&RunController::scenarioGenerated),success(&controller,&RunController::succeeded);
        QVERIFY(controller.startGenerated(request(QDir(artifacts).filePath("bad-native-config")),generatorConfig(data)));QVERIFY(idle(controller));
        QCOMPARE(failed.size(),1);QVERIFY(failed.first().first().toString().contains("tasks.count"));QVERIFY(generated.empty());QVERIFY(success.empty());
        for (const auto* mode : {"generation-missing","generation-incomplete"}) {
            failed.clear();qputenv("TELEMETRY_GUI_FAKE_MODE",mode);auto req = request(QDir(artifacts).filePath(mode));req.program = fake;
            QVERIFY(controller.startGenerated(req,generatorConfig(parameters())));QVERIFY(idle(controller));QCOMPARE(failed.size(),1);QVERIFY(generated.empty());QVERIFY(success.empty());
        }
    }
    void bytePreservingEdits() {
        QTemporaryDir temp;const auto path = fixture(temp.path());auto original = read(path);
        original.replace("\"crashes\"","\"cr\\u0061shes\"");original.replace("\"delivery_trace\"","\"delivery_tra\\u0063e\"");
        original.replace("\"work\": 4","\"work\": 4.0000000000000000000000000000000001");original.prepend("\xEF\xBB\xBF");write(path,original);
        const auto document = readScenarioDocument(path);const auto edited = editedScenario(document,{{1,7},{2,7}},"new.csv");
        auto after = document;after.bytes = edited;
        QCOMPARE(editedScenario(after,{},"same.csv"),editedScenario(document,{},"same.csv"));
        QVERIFY(edited.contains("4.0000000000000000000000000000000001"));QCOMPARE(read(path),original);
        const auto decoded = nlohmann::json::parse(edited.toStdString());QCOMPARE(decoded.at("crashes").size(),std::size_t(2));
    }
    void validatedCopyAndRun() {
        const auto root = QDir(artifacts).filePath("saved-copy");const auto path = fixture(root);const auto document = readScenarioDocument(path);
        const auto target = QDir(QFileInfo(path).absolutePath()).filePath("копия сценария.json"); // Same folder must not overwrite original CSV.
        RunController controller;QSignalSpy saved(&controller,&RunController::copySaved),failed(&controller,&RunController::failed),success(&controller,&RunController::succeeded);
        QVERIFY(controller.saveCopy(request(root),document,{{1,7},{2,40}},target));QVERIFY(idle(controller));QCOMPARE(failed.size(),0);QCOMPARE(saved.size(),1);
        const auto copy = readScenarioDocument(target);QCOMPARE(copy.failures,(QVector<FailureSpec>{{1,7},{2,40}}));
        QCOMPARE(copy.deliveryHash,document.deliveryHash);QVERIFY(copy.delivery != document.delivery);
        QCOMPARE(editedScenario(copy,{},"same.csv"),editedScenario(document,{},"same.csv"));QCOMPARE(sha256(path),document.sourceHash);QCOMPARE(sha256(document.delivery),document.deliveryHash);
        QVERIFY(controller.start(request(root,target)));QVERIFY(idle(controller));QCOMPARE(success.size(),1);QCOMPARE(failed.size(),0);
        QCOMPARE(qvariant_cast<RunResult>(success.first().first()).prepared.input.scenario,target);
    }
    void copyFailuresDoNotPublishOrOverwrite() {
        QTemporaryDir temp;const auto path = fixture(temp.path());const auto document = readScenarioDocument(path);RunController controller;QSignalSpy failed(&controller,&RunController::failed),saved(&controller,&RunController::copySaved);
        QVERIFY(controller.saveCopy(request(temp.path()),document,{},path));QVERIFY(idle(controller));QCOMPARE(failed.size(),1);QCOMPARE(sha256(path),document.sourceHash);
        const auto target = QDir(temp.path()).filePath("new.json");auto req = request(temp.path());req.program = fake;qputenv("TELEMETRY_GUI_FAKE_MODE","copy-validate-fail");
        failed.clear();QVERIFY(controller.saveCopy(req,document,{},target));QVERIFY(idle(controller));QCOMPARE(failed.size(),1);QVERIFY(saved.empty());QVERIFY(!QFileInfo::exists(target));
        qunsetenv("TELEMETRY_GUI_FAKE_MODE");write(document.delivery,read(document.delivery)+"\n");failed.clear();
        QVERIFY(controller.saveCopy(request(temp.path()),document,{},target));QVERIFY(idle(controller));QCOMPARE(failed.size(),1);QVERIFY(!QFileInfo::exists(target));
        QVERIFY(failed.first().first().toString().contains("изменились"));
    }
    void publicationRaceDoesNotOverwrite() {
        QTemporaryDir temp;const auto path = fixture(temp.path());const auto document = readScenarioDocument(path);const auto target = QDir(temp.path()).filePath("race.json");
        const auto copy = prepareCopy(request(temp.path()),document,{},target);write(target,"keep");
        QVERIFY(rejected([&] { publishCopy(copy); }));QCOMPARE(read(target),QByteArray("keep"));QCOMPARE(sha256(path),document.sourceHash);
    }
    void formWidgetsAndWindowWorkflow() {
        auto data = parameters();ScenarioFormDialog form(data);form.show();
        form.findChild<QTableWidget*>("nodesTable")->setRowCount(0);QTest::mouseClick(form.findChild<QPushButton*>("createScenario"),Qt::LeftButton);
        QVERIFY(form.findChild<QLabel*>("formError")->isVisible());QVERIFY(form.config().isEmpty());form.setData(data);form.findChild<QLabel*>("formError")->hide();
        auto* nodes = form.findChild<QTableWidget*>("nodesTable");
        QTest::mouseClick(form.findChild<QPushButton*>("nodesTableAdd"),Qt::LeftButton);QCOMPARE(nodes->rowCount(),3);nodes->selectRow(2);
        QTest::mouseClick(form.findChild<QPushButton*>("nodesTableRemove"),Qt::LeftButton);QCOMPARE(nodes->rowCount(),2);
        auto* tabs = form.findChild<QTabWidget*>();QVERIFY(tabs);
        for (int index = 0; index < tabs->count(); ++index) {
            tabs->setCurrentIndex(index);QTest::qWait(30);
            QVERIFY(form.grab().save(QDir(artifacts).filePath(QString("new-scenario-tab-%1.png").arg(index+1))));
        }
        tabs->setCurrentIndex(0);QTest::qWait(30);QVERIFY(form.grab().save(QDir(artifacts).filePath("new-scenario-form.png")));form.hide();
        MainWindow window(program,QDir(artifacts).filePath("window-data"));window.show();QSignalSpy success(window.controller(),&RunController::succeeded),failed(window.controller(),&RunController::failed),copied(window.controller(),&RunController::copySaved);
        QTimer::singleShot(100,&window,[data] {
            if (auto* dialog = qobject_cast<ScenarioFormDialog*>(QApplication::activeModalWidget())) {
                dialog->setData(data);QTest::mouseClick(dialog->findChild<QPushButton*>("createScenario"),Qt::LeftButton);
            }
        });
        QTest::mouseClick(window.findChild<QPushButton*>("newScenario"),Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(success.size(),1,20000);QCOMPARE(failed.size(),0);
        const auto generated = window.findChild<QLineEdit*>("scenarioPath")->text();QVERIFY(QFileInfo::exists(generated));const auto doc = readScenarioDocument(generated);
        QVERIFY(window.openScenario(generated));QTest::mouseClick(window.findChild<QPushButton*>("startRun"),Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(success.size(),2,20000);
        const auto target = QDir(artifacts).filePath("копия из окна.json");
        QTimer automation;automation.setInterval(50);bool scheduled = false;
        connect(&automation,&QTimer::timeout,&window,[&] {
            if (auto* editor = qobject_cast<FailureDialog*>(QApplication::activeModalWidget())) {
                auto* grid = editor->findChild<QTableWidget*>("copyFailuresTable");grid->item(0,1)->setText("7");
                editor->grab().save(QDir(artifacts).filePath("failure-editor.png"));
                QTest::mouseClick(editor->findChild<QPushButton*>("saveScenarioCopy"),Qt::LeftButton);
            } else if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()); dialog && !scheduled) {
                scheduled = true;dialog->setDirectory(QFileInfo(target).absolutePath());
                QTimer::singleShot(300,dialog,[dialog,target] {
                    if (auto* edit = dialog->findChild<QLineEdit*>("fileNameEdit")) { edit->setText(QFileInfo(target).fileName());QTest::keyClick(edit,Qt::Key_Return); }
                });QTimer::singleShot(3000,dialog,[dialog] { dialog->reject(); });
            }
        });automation.start();QTest::mouseClick(window.findChild<QPushButton*>("editFailures"),Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(copied.size(),1,20000);automation.stop();QCOMPARE(failed.size(),0);
        const auto copy = readScenarioDocument(target);QCOMPARE(copy.failures.first().at,7);QCOMPARE(copy.deliveryHash,doc.deliveryHash);
        QCOMPARE(editedScenario(copy,{},"same.csv"),editedScenario(doc,{},"same.csv"));
        QTest::mouseClick(window.findChild<QPushButton*>("startRun"),Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(success.size(),3,20000);
        window.playback()->findChild<QComboBox*>("selectedTask")->setCurrentText("3");while (window.playback()->controller()->step()) {}
        QTest::qWait(100);
        const auto history = window.findChild<QTableWidget*>("taskHistory");const auto outer = window.findChild<QScrollArea*>("playbackScroll");
        const auto rect = history->visualItemRect(history->item(history->rowCount()-1,4));
        QVERIFY(outer->viewport()->rect().contains(QRect(history->viewport()->mapTo(outer->viewport(),rect.topLeft()),rect.size())));
        QVERIFY(window.grab().save(QDir(artifacts).filePath("window-saved-copy.png")));
    }
};
int main(int argc,char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);QApplication app(argc,argv);if (argc != 5 && argc != 6) { return 2; }
    program = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();fake = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();source = QFileInfo(QString::fromLocal8Bit(argv[3])).absoluteFilePath();
    artifacts = QDir(QString::fromLocal8Bit(argv[4])).absoluteFilePath("check-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    qInfo("Scenario editor artifacts: %s",qPrintable(artifacts));EditorTests tests;char* args[] = {argv[0],argc == 6 ? argv[5] : nullptr,nullptr};return QTest::qExec(&tests,argc == 6 ? 2 : 1,args);
}
#include "editor_tests.moc"
