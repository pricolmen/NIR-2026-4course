#include "playback_widget.hpp"
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace telemetry::gui {
namespace {
void cell(QTableWidget* table, int row, int column, const QString& text) {
    auto* item = new QTableWidgetItem(text);item->setFlags(item->flags() & ~Qt::ItemIsEditable);table->setItem(row,column,item);
}
}
PlaybackWidget::PlaybackWidget(QWidget* parent) : QWidget(parent),m_replay(this) {
    auto* layout = new QVBoxLayout(this);layout->setContentsMargins(4,4,4,4);layout->setSpacing(4);layout->setSizeConstraint(QLayout::SetMinimumSize);
    m_identity = new QLabel("Сохранённый прогон не выбран",this);m_identity->setTextFormat(Qt::PlainText);m_identity->setWordWrap(true);layout->addWidget(m_identity);
    m_time = new QLabel("Модельное время: 0 мкс",this);m_time->setObjectName("modelTime");
    auto font = m_time->font();font.setPointSize(15);font.setBold(true);m_time->setFont(font);layout->addWidget(m_time);
    auto* controls = new QHBoxLayout;
    m_play = new QPushButton("Воспроизвести",this);m_play->setObjectName("play");controls->addWidget(m_play);
    m_pause = new QPushButton("Пауза",this);m_pause->setObjectName("pause");controls->addWidget(m_pause);
    m_step = new QPushButton("Шаг",this);m_step->setObjectName("step");controls->addWidget(m_step);
    auto* reset = new QPushButton("Сначала",this);reset->setObjectName("resetReplay");controls->addWidget(reset);
    auto* speed = new QComboBox(this);speed->setObjectName("playbackSpeed");
    for (double value : {0.25,0.5,1.,2.,4.,8.}) { speed->addItem("×"+QString::number(value),value); }
    speed->setCurrentIndex(2);controls->addWidget(speed);layout->addLayout(controls);
    speed->setToolTip("×1: весь горизонт за 8 секунд. Шаг применяет ровно одну запись журнала.");
    m_position = new QLabel("Загрузите результат расчёта",this);m_position->setObjectName("replayPosition");m_position->setWordWrap(true);layout->addWidget(m_position);
    m_nodes = new QTableWidget(0,3,this);m_nodes->setObjectName("nodeStates");m_nodes->setHorizontalHeaderLabels({"Узел","Фактическое состояние","Оценка детектора"});
    m_nodes->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);m_nodes->verticalHeader()->hide();m_nodes->setMinimumHeight(110);m_nodes->setMaximumHeight(210);layout->addWidget(m_nodes);
    auto* taskRow = new QHBoxLayout;taskRow->addWidget(new QLabel("История задачи",this));
    m_task = new QComboBox(this);m_task->setObjectName("selectedTask");taskRow->addWidget(m_task);taskRow->addStretch();layout->addLayout(taskRow);
    m_taskStatus = new QLabel(this);m_taskStatus->setObjectName("taskReplayStatus");m_taskStatus->setWordWrap(true);layout->addWidget(m_taskStatus);
    m_history = new QTableWidget(0,5,this);m_history->setObjectName("taskHistory");
    m_history->setHorizontalHeaderLabels({"№ записи","Время, мкс","Попытка","Узел","Событие / исход"});
    m_history->verticalHeader()->hide();m_history->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_history->horizontalHeader()->setStretchLastSection(true);m_history->setMinimumHeight(120);m_history->setMaximumHeight(150);m_history->setWordWrap(true);layout->addWidget(m_history,1);
    m_history->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_play,&QPushButton::clicked,&m_replay,&ReplayController::play);connect(m_pause,&QPushButton::clicked,&m_replay,&ReplayController::pause);
    connect(m_step,&QPushButton::clicked,this,[this] { m_replay.step(); });connect(reset,&QPushButton::clicked,&m_replay,&ReplayController::reset);
    connect(speed,&QComboBox::currentIndexChanged,this,[this,speed] { m_replay.setSpeed(speed->currentData().toDouble()); });
    connect(m_task,&QComboBox::currentIndexChanged,this,[this] { refresh(); });
    connect(&m_replay,&ReplayController::changed,this,&PlaybackWidget::refresh);
    connect(&m_replay,&ReplayController::playingChanged,this,[this] { refresh(); });refresh();
    for (auto* item : {m_identity,m_time,m_position,m_taskStatus}) { item->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed); }
}
void PlaybackWidget::setData(QSharedPointer<const ReplayData> data, const QString& identity) {
    m_identity->setText(identity.isEmpty() ? "Сохранённый журнал" : identity);
    m_visibleRows = -1;
    m_task->blockSignals(true);m_task->clear();m_nodes->setRowCount(0);
    if (data) {
        for (const auto& task : data->tasks) { m_task->addItem(task); }
        m_nodes->setRowCount(static_cast<int>(data->nodes.size()));
        m_nodes->setFixedHeight(static_cast<int>(std::clamp<qsizetype>(data->nodes.size()*31+32,80,180)));
        for (qsizetype i = 0; i < data->nodes.size(); ++i) { cell(m_nodes,static_cast<int>(i),0,data->nodes[i]); }
    }
    m_task->blockSignals(false);m_replay.setData(std::move(data));
}
void PlaybackWidget::refresh() {
    const auto& state = m_replay.state();const auto data = m_replay.data();
    m_time->setText(QString("Модельное время: %1 мкс").arg(state.time));
    m_position->setText(!data ? "Сначала выполните расчёт" : QString("Запись %1 из %2 · В общей очереди: %3 · ×1: горизонт за 8 с%4")
        .arg(state.record).arg(data->events.size()).arg(state.queued.size()).arg(m_replay.finished() ? " · Просмотр завершён" : ""));
    m_play->setEnabled(data && !m_replay.playing() && !m_replay.finished());m_pause->setEnabled(m_replay.playing());m_step->setEnabled(data && !m_replay.finished());
    if (!data) { return; }
    for (qsizetype i = 0; i < data->nodes.size(); ++i) {
        const auto view = state.nodes.value(data->nodes[i]);cell(m_nodes,static_cast<int>(i),1,view.crashed ? "Отказал" : "Работоспособен");
        cell(m_nodes,static_cast<int>(i),2,view.detector);
    }
    const auto task = state.tasks.value(m_task->currentText());
    m_taskStatus->setText(m_task->count() == 0 ? "Задач в сценарии нет" : task.status + (task.attempt.isEmpty() ? "" : " · Последняя попытка " + task.attempt + " · Узел " + task.node));
    if (m_visibleTask == m_task->currentText() && m_visibleRows == task.history.size()) { return; }
    m_visibleTask = m_task->currentText();m_visibleRows = task.history.size();
    m_history->setRowCount(static_cast<int>(task.history.size()));
    for (qsizetype i = 0; i < task.history.size(); ++i) {
        const auto& row = task.history[i];const auto n = static_cast<int>(i);
        cell(m_history,n,0,QString::number(row.record));cell(m_history,n,1,QString::number(row.time));
        cell(m_history,n,2,row.attempt);cell(m_history,n,3,row.node);cell(m_history,n,4,row.event);
    }
    m_history->resizeRowsToContents();m_history->scrollToBottom();
    QTimer::singleShot(0,this,[this] { m_history->scrollToBottom(); });
}
} // namespace telemetry::gui
