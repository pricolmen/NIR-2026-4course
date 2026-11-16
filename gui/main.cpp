#include "main_window.hpp"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

int main(int argc, char** argv) {
    QApplication app(argc,argv);
    QCoreApplication::setApplicationName("telemetry_gui");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCommandLineParser parser;parser.setApplicationDescription("Локальный интерфейс запуска telemetry_sim");
    parser.addHelpOption();parser.addVersionOption();
    parser.addOption({"scenario","Готовый scenario.json для открытия","file"});
    parser.addOption({"simulator","Исполняемый файл telemetry_sim","file"});
    parser.addOption({"results-root","Каталог новых прогонов","directory",QDir::current().filePath("results/gui-runs")});
    parser.process(app);
#ifdef Q_OS_WIN
    const auto executable = "telemetry_sim.exe";
#else
    const auto executable = "telemetry_sim";
#endif
    auto program = parser.value("simulator");
    if (program.isEmpty()) {
        program = QDir(QCoreApplication::applicationDirPath()).filePath(executable);
        if (!QFileInfo::exists(program)) { program = QStandardPaths::findExecutable(executable); }
    }
    telemetry::gui::MainWindow window(program,parser.value("results-root"));
    if (parser.isSet("scenario")) { window.openScenario(parser.value("scenario")); }
    window.show();return app.exec();
}
