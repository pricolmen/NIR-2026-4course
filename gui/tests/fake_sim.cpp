// Protocol fault injector for tests; copies one real CLI fixture, no simulation.
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>

namespace {
void write(const QString& path, const QByteArray& data) {
    QFile file(path);if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) { throw 1; }
}
QByteArray read(const QString& path) {
    QFile file(path);if (!file.open(QIODevice::ReadOnly)) { throw 1; }return file.readAll();
}
void copyTree(const QString& source, const QString& target) {
    QDirIterator it(source,QDir::Files | QDir::NoDotAndDotDot,QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto path = it.next();const auto out = QDir(target).filePath(QDir(source).relativeFilePath(path));
        QDir().mkpath(QFileInfo(out).absolutePath());if (!QFile::copy(path,out)) { throw 1; }
    }
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);const auto args = app.arguments();
    const auto mode = qEnvironmentVariable("TELEMETRY_GUI_FAKE_MODE");
    if (args.value(1) == "generate") {
        if (mode == "generation-missing") { return 0; }
        if (mode == "generation-incomplete") {
            const auto out = args.value(args.indexOf("--out")+1);QDir().mkpath(out);write(QDir(out).filePath("generation_manifest.json"),"{\"status\":\"incomplete\"}");return 0;
        }
        QTextStream(stderr) << "Контрольная ошибка generate\n";return 1;
    }
    if (mode == "slow") { QThread::msleep(400); }
    if (args.value(1) == "validate") {
        if (mode == "copy-validate-fail") { QTextStream(stderr) << "Контрольный отказ validate копии\n";return 1; }
        if (mode == "mutate-input") {
            const auto file = args.value(args.indexOf("--scenario")+1);write(file,read(file) + "\n");
        }
        QTextStream(stdout) << "validation fixture\n";return 0;
    }
    if (args.value(1) != "run") { return 2; }
    if (mode == "exit1" || mode == "exit2") {
        QTextStream(stderr) << "Контрольная ошибка выполнения: код " << mode.right(1) << '\n';return mode == "exit1" ? 1 : 2;
    }
    if (mode == "crash") { std::abort(); }
    if (mode == "no-files") { return 0; }
    try {
        const auto out = args.value(args.indexOf("--out")+1);
        copyTree(qEnvironmentVariable("TELEMETRY_GUI_FIXTURE_RUN"),out);
        const auto scenario = args.value(args.indexOf("--scenario")+1);
        const auto sourceJson = read(scenario);
        const auto trace = QJsonDocument::fromJson(sourceJson).object().value("delivery_trace").toString();
        write(QDir(out).filePath("input/scenario.json"),sourceJson);
        write(QDir(out).filePath("input/delivery.csv"),read(QDir(QFileInfo(scenario).absolutePath()).filePath(trace)));
        const auto manifestPath = QDir(out).filePath("manifest.json");
        auto manifest = QJsonDocument::fromJson(read(manifestPath)).object();
        if (mode == "incomplete") { manifest["status"] = "incomplete"; }
        if (mode == "wrong-variant") { manifest["variant"] = "adaptive_ll"; }
        write(manifestPath,QJsonDocument(manifest).toJson());
        const auto journal = QDir(out).filePath("events.jsonl");
        auto lines = read(journal).split('\n');if (lines.last().isEmpty()) { lines.removeLast(); }
        if (mode == "no-marker") { lines.removeLast(); }
        if (mode == "wrong-horizon") {
            auto last = QJsonDocument::fromJson(lines.last()).object();last["time_ticks"] = 39;
            lines.last() = QJsonDocument(last).toJson(QJsonDocument::Compact);
        }
        if (mode == "after-marker") { lines.append(lines.first()); }
        if (mode == "journal-checksum") { lines.first().append(' '); }
        write(journal,lines.join('\n') + '\n');
        if (mode == "wrong-input") { write(QDir(out).filePath("input/scenario.json"),"{}"); }
        if (mode == "bad-metrics") { write(QDir(out).filePath("metrics.json"),"{"); }
        return 0;
    } catch (...) { QTextStream(stderr) << "Fault fixture setup failed\n";return 1; }
}
