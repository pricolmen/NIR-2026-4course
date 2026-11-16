#include "main_window.hpp"
#include "playback_widget.hpp"
#include "scenario_dialogs.hpp"
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QScrollArea>
#include <QTableWidget>
#include <QHeaderView>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>

namespace telemetry::gui {
namespace {
QLabel* label(const QString& text, QWidget* parent = nullptr) {
    auto* item = new QLabel(text,parent);
    item->setTextFormat(Qt::PlainText);item->setWordWrap(true);
    item->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return item;
}
}
MainWindow::MainWindow(QString simulator, QString resultsRoot, QWidget* parent)
    : QMainWindow(parent),m_controller(this) {
    setWindowTitle("Отказоустойчивая обработка телеметрии");resize(1320,940);
    auto* body = new QWidget(this);setCentralWidget(body);
    auto* layout = new QVBoxLayout(body);layout->setContentsMargins(14,10,14,10);layout->setSpacing(6);
    auto* title = label("Распределённая обработка телеметрии");
    auto font = title->font();font.setPointSize(18);font.setBold(true);title->setFont(font);layout->addWidget(title);
    layout->addWidget(label("Готовый сценарий → проверка → расчёт → сохранённый результат"));
    auto* panes = new QSplitter(Qt::Horizontal,body);
    auto* inputScroll = new QScrollArea(panes);inputScroll->setWidgetResizable(true);inputScroll->setMinimumWidth(300);
    auto* inputs = new QWidget(inputScroll);inputScroll->setWidget(inputs);auto* inputLayout = new QVBoxLayout(inputs);
    auto* playbackBox = new QGroupBox("Ход моделирования — просмотр сохранённого журнала",panes);
    auto* playbackLayout = new QVBoxLayout(playbackBox);
    auto* playbackScroll = new QScrollArea(playbackBox);playbackScroll->setObjectName("playbackScroll");playbackScroll->setWidgetResizable(true);
    m_playback = new PlaybackWidget(playbackScroll);playbackScroll->setWidget(m_playback);playbackLayout->addWidget(playbackScroll);
    panes->addWidget(inputScroll);panes->addWidget(playbackBox);panes->setStretchFactor(0,0);panes->setStretchFactor(1,1);panes->setSizes({450,850});
    layout->addWidget(panes,1);

    auto* settings = new QGroupBox("Настройка запуска",inputs);
    auto* form = new QFormLayout(settings);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* scenarioActions = new QWidget(settings);auto* actions = new QHBoxLayout(scenarioActions);actions->setContentsMargins(0,0,0,0);
    auto* create = new QPushButton("Новый сценарий…",scenarioActions);create->setObjectName("newScenario");actions->addWidget(create);
    m_editFailures = new QPushButton("Отказы / копия…",scenarioActions);m_editFailures->setObjectName("editFailures");m_editFailures->setEnabled(false);actions->addWidget(m_editFailures);
    form->addRow(scenarioActions);
    m_scenario = new QLineEdit(settings);m_scenario->setObjectName("scenarioPath");m_scenario->setReadOnly(true);
    m_scenario->setPlaceholderText("Откройте scenario.json вместе с его трассой CSV");
    auto* open = new QPushButton("Открыть JSON…",settings);open->setObjectName("openScenario");
    auto* sourceRow = new QWidget(settings);auto* sourceLayout = new QHBoxLayout(sourceRow);sourceLayout->setContentsMargins(0,0,0,0);
    sourceLayout->addWidget(m_scenario,1);sourceLayout->addWidget(open);form->addRow("Сценарий",sourceRow);
    m_preview = label("Сценарий не выбран",settings);m_preview->setObjectName("inputPreview");form->addRow(m_preview);
    connect(open,&QPushButton::clicked,this,[this] {
        const auto path = QFileDialog::getOpenFileName(this,"Открыть готовый сценарий",m_scenario->text(),"Сценарии JSON (*.json)");
        if (!path.isEmpty()) { openScenario(path); }
    });
    m_detector = new QComboBox(settings);m_detector->setObjectName("detector");
    m_detector->addItem(detectorName("fixed"),"fixed");m_detector->addItem(detectorName("adaptive"),"adaptive");
    m_scheduler = new QComboBox(settings);m_scheduler->setObjectName("scheduler");
    m_scheduler->addItem(schedulerName("rr"),"rr");m_scheduler->addItem(schedulerName("ll"),"ll");
    form->addRow("Детектор",m_detector);form->addRow("Распределение",m_scheduler);
    m_program = new QLineEdit(simulator,settings);m_program->setObjectName("simulatorPath");
    auto* chooseProgram = new QPushButton("Выбрать…",settings);
    auto* programRow = new QWidget(settings);auto* programLayout = new QHBoxLayout(programRow);programLayout->setContentsMargins(0,0,0,0);
    programLayout->addWidget(m_program,1);programLayout->addWidget(chooseProgram);form->addRow("telemetry_sim",programRow);
    connect(chooseProgram,&QPushButton::clicked,this,[this] {
        const auto path = QFileDialog::getOpenFileName(this,"Исполняемый файл telemetry_sim",m_program->text());
        if (!path.isEmpty()) { m_program->setText(path); }
    });
    m_output = new QLineEdit(resultsRoot,settings);m_output->setObjectName("resultsRoot");
    auto* chooseOutput = new QPushButton("Выбрать…",settings);
    auto* outputRow = new QWidget(settings);auto* outputLayout = new QHBoxLayout(outputRow);outputLayout->setContentsMargins(0,0,0,0);
    outputLayout->addWidget(m_output,1);outputLayout->addWidget(chooseOutput);form->addRow("Каталог прогонов",outputRow);
    connect(chooseOutput,&QPushButton::clicked,this,[this] {
        const auto path = QFileDialog::getExistingDirectory(this,"Каталог новых прогонов",m_output->text());
        if (!path.isEmpty()) { m_output->setText(path); }
    });
    connect(create,&QPushButton::clicked,this,[this] {
        ScenarioFormDialog dialog(m_lastForm,this);const auto accepted = dialog.exec() == QDialog::Accepted;m_lastForm = dialog.data();
        if (!accepted) { return; }
        m_error->hide();m_diagnostics->clear();m_playback->controller()->pause();
        m_controller.startGenerated({{},m_program->text(),m_output->text(),m_detector->currentData().toString(),m_scheduler->currentData().toString()},dialog.config());
    });
    connect(m_editFailures,&QPushButton::clicked,this,[this] {
        try {
            const auto document = readScenarioDocument(m_scenario->text());FailureDialog dialog(document,this);
            if (dialog.exec() != QDialog::Accepted) { return; }
            const auto suggested = QDir(QFileInfo(document.source).absolutePath()).filePath(QFileInfo(document.source).completeBaseName()+"_copy.json");
            const auto destination = QFileDialog::getSaveFileName(this,"Сохранить новую копию сценария",suggested,"JSON (*.json)",nullptr,QFileDialog::DontConfirmOverwrite);
            if (destination.isEmpty()) { return; }
            m_error->hide();m_diagnostics->clear();m_playback->controller()->pause();
            m_controller.saveCopy({m_scenario->text(),m_program->text(),m_output->text(),m_detector->currentData().toString(),m_scheduler->currentData().toString()},document,dialog.failures(),destination);
        } catch (const std::exception& error) { m_error->setText(QString::fromUtf8(error.what()));m_error->show(); }
    });
    inputLayout->addWidget(settings);
    m_inputWidgets = {open,create,m_editFailures,m_detector,m_scheduler,m_program,chooseProgram,m_output,chooseOutput};
    auto* runRow = new QHBoxLayout;
    m_start = new QPushButton("Запустить расчёт",body);m_start->setObjectName("startRun");m_start->setMinimumHeight(36);
    m_start->setEnabled(false);runRow->addWidget(m_start);
    m_status = label("Выберите сценарий",body);m_status->setObjectName("runStatus");runRow->addWidget(m_status,1);
    m_progress = new QProgressBar(body);m_progress->setRange(0,0);m_progress->setTextVisible(false);m_progress->setFixedWidth(110);m_progress->hide();runRow->addWidget(m_progress);
    inputLayout->addLayout(runRow);
    m_error = label({},inputs);m_error->setObjectName("runError");m_error->setStyleSheet("color: #b02929;");m_error->hide();inputLayout->addWidget(m_error);
    connect(m_start,&QPushButton::clicked,this,[this] {
        m_playback->controller()->pause();
        m_error->clear();m_error->hide();m_diagnostics->clear();
        m_controller.start({m_scenario->text(),m_program->text(),m_output->text(),
            m_detector->currentData().toString(),m_scheduler->currentData().toString()});
    });
    auto* results = new QGroupBox("Итоги полного прогона",body);auto* resultLayout = new QVBoxLayout(results);resultLayout->setSpacing(4);
    resultLayout->addWidget(label("Эти показатели относятся ко всему прогону и не меняются при паузе или шаге просмотра."));
    auto* historyRow = new QHBoxLayout;
    m_history = new QComboBox(results);m_history->setObjectName("runHistory");m_history->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_history->setMinimumContentsLength(30);m_history->setEnabled(false);
    auto* folder = new QPushButton("Открыть файлы прогона",results);folder->setEnabled(false);
    historyRow->addWidget(m_history,1);historyRow->addWidget(folder);resultLayout->addLayout(historyRow);
    m_resultIdentity = label("Результатов пока нет. Выбор другого сценария не меняет уже сохранённые итоги.",results);
    m_resultIdentity->setObjectName("resultIdentity");resultLayout->addWidget(m_resultIdentity);
    m_summary = new QPlainTextEdit(results);m_summary->setObjectName("runSummary");m_summary->setReadOnly(true);m_summary->setFixedHeight(96);
    m_summary->setPlaceholderText("После успешного расчёта здесь появятся показатели из metrics.json.");
    auto* metricRow = new QHBoxLayout;metricRow->addWidget(m_summary,1);
    m_nodeMetrics = new QTableWidget(0,4,results);m_nodeMetrics->setObjectName("nodeMetrics");
    m_nodeMetrics->setHorizontalHeaderLabels({"Узел","Классификация","Обнаружение","Восстановление"});
    m_nodeMetrics->verticalHeader()->hide();m_nodeMetrics->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_nodeMetrics->setFixedHeight(96);metricRow->addWidget(m_nodeMetrics,1);resultLayout->addLayout(metricRow);
    layout->addWidget(results);
    connect(m_history,&QComboBox::currentIndexChanged,this,&MainWindow::updateResult);
    connect(folder,&QPushButton::clicked,this,[this] {
        const auto index = m_history->currentIndex();
        if (index >= 0 && index < m_results.size()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_results[index].prepared.directory));
        }
    });
    auto* diagnostics = new QGroupBox("Диагностика запуска",body);auto* diagnosticLayout = new QVBoxLayout(diagnostics);
    m_diagnostics = new QPlainTextEdit(diagnostics);m_diagnostics->setObjectName("diagnostics");m_diagnostics->setReadOnly(true);
    m_diagnostics->setMaximumBlockCount(2000);m_diagnostics->setMaximumHeight(110);diagnosticLayout->addWidget(m_diagnostics);inputLayout->addWidget(diagnostics);inputLayout->addStretch();
    connect(&m_controller,&RunController::diagnostic,this,[this](const QString& text) {
        auto cursor = m_diagnostics->textCursor();cursor.movePosition(QTextCursor::End);cursor.insertText(text + (text.endsWith('\n') ? "" : "\n"));
        m_diagnostics->setTextCursor(cursor);
    });
    connect(&m_controller,&RunController::stageChanged,m_status,&QLabel::setText);
    connect(&m_controller,&RunController::busyChanged,this,[this](bool busy) {
        for (auto* widget : m_inputWidgets) { widget->setEnabled(!busy); }
        m_editFailures->setEnabled(!busy && !m_scenario->text().isEmpty());
        m_start->setEnabled(!busy && !m_scenario->text().isEmpty());m_progress->setVisible(busy);
    });
    connect(&m_controller,&RunController::failed,this,[this](const QString& message) { m_error->setText(message);m_error->show(); });
    connect(&m_controller,&RunController::scenarioGenerated,this,[this](const QString& path) { showScenario(path); });
    connect(&m_controller,&RunController::copySaved,this,[this](const QString& path) { openScenario(path); });
    connect(&m_controller,&RunController::succeeded,this,[this,folder](const RunResult& result) {
        m_results.append(result);
        const auto text = QFileInfo(result.prepared.session).fileName() + " · " + QFileInfo(result.prepared.input.scenario).fileName()
            + " · " + result.prepared.request.detector + "/" + result.prepared.request.scheduler;
        m_history->addItem(text);m_history->setCurrentIndex(m_history->count()-1);m_history->setEnabled(true);folder->setEnabled(true);
        updateResult(m_history->currentIndex());
    });
}
bool MainWindow::openScenario(const QString& path) {
    if (m_controller.busy()) { return false; }
    return showScenario(path);
}
bool MainWindow::showScenario(const QString& path) {
    m_scenario->setText(QFileInfo(path).absoluteFilePath());m_start->setEnabled(!m_controller.busy());m_editFailures->setEnabled(!m_controller.busy());m_error->hide();
    try {
        const auto input = inspectInput(path);
        m_preview->setText(QString("Узлов: %1 · Задач: %2 · Горизонт: %3 мкс\nТрасса: %4")
            .arg(input.nodes).arg(input.tasks).arg(input.horizon).arg(input.delivery));
        m_status->setText("Сценарий выбран — полная проверка перед расчётом");return true;
    } catch (const std::exception& error) {
        m_preview->setText("Проверьте JSON и связанную трассу CSV");m_error->setText(QString::fromUtf8(error.what()));m_error->show();
        m_status->setText("Ошибка чтения — исправьте файлы и повторите запуск");return false;
    }
}
void MainWindow::updateResult(int index) {
    if (index < 0 || index >= m_results.size()) { return; }
    const auto& result = m_results[index];const auto& prepared = result.prepared;const auto& metrics = result.metrics;
    const auto runId = QFileInfo(prepared.session).fileName().right(36);
    m_playback->setData(result.playback,QString("Просмотр: %1 · %2/%3\nПрогон %4")
        .arg(QFileInfo(prepared.input.scenario).fileName(),prepared.request.detector,prepared.request.scheduler,runId));
    m_playback->setToolTip("Исходный JSON: " + prepared.input.scenario + "\nКаталог результата: " + prepared.directory);
    m_nodeMetrics->setRowCount(result.playback ? static_cast<int>(result.playback->metrics.size()) : 0);
    if (result.playback) {
        const QMap<QString,QString> classifications{{"true_positive","Обнаружен отказ"},{"true_negative","Исправен"},{"no_failure","Без физического отказа"},
            {"false_positive","Ложное исключение"},{"false_negative","Отказ не обнаружен"},{"not_detected","Отказ не обнаружен"},{"crash_after_false_exclusion","Отказ после ложного исключения"}};
        for (qsizetype i = 0; i < result.playback->metrics.size(); ++i) {
            const auto& node = result.playback->metrics[i];
            const QStringList cells{node.node,classifications.value(node.classification,node.classification),node.detection,node.recovery};
            for (int column = 0; column < cells.size(); ++column) {
                auto* item = new QTableWidgetItem(cells[column]);item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                m_nodeMetrics->setItem(static_cast<int>(i),column,item);
            }
        }
        m_nodeMetrics->resizeRowsToContents();
    }
    m_resultIdentity->setText(QString("Сценарий этого результата: %1 · Прогон %7\n%2 · %3 · Узлов: %4 · Задач: %5 · Горизонт достигнут: %6 мкс")
        .arg(QFileInfo(prepared.input.scenario).fileName(),detectorName(prepared.request.detector),schedulerName(prepared.request.scheduler))
        .arg(prepared.input.nodes).arg(prepared.input.tasks).arg(prepared.input.horizon).arg(runId));
    m_resultIdentity->setToolTip("Исходный JSON: " + prepared.input.scenario + "\nКаталог выбранного прогона: " + prepared.directory);
    m_summary->setPlainText(QString("Завершено задач: %1 · Незавершено задач: %2\nЛожных исключений: %3 · Средняя задержка: %4\nОбнаружение: %5 · Восстановление: %6\nP95 задержки: %7")
        .arg(metrics.value("completed").toInteger()).arg(metrics.value("unfinished").toInteger()).arg(metrics.value("false_positives").toInteger())
        .arg(durationText(metrics.value("mean_latency").toObject()),durationText(metrics.value("mean_detection").toObject()),durationText(metrics.value("mean_recovery").toObject()))
        .arg(durationText(metrics.value("p95_latency").toObject())));
}
} // namespace telemetry::gui
