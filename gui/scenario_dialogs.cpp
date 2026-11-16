#include "scenario_dialogs.hpp"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace telemetry::gui {
namespace {
QLabel* explanation(const QString& value) {
    auto* label = new QLabel(value);label->setWordWrap(true);label->setTextFormat(Qt::PlainText);return label;
}
QTableWidget* table(QVBoxLayout* parent,const QString& title,const QString& name,const QStringList& columns,const QStringList& defaults) {
    auto* group = new QGroupBox(title);auto* layout = new QVBoxLayout(group);
    auto* grid = new QTableWidget(0,static_cast<int>(columns.size()),group);grid->setObjectName(name);grid->setHorizontalHeaderLabels(columns);
    grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);grid->setSelectionBehavior(QAbstractItemView::SelectRows);
    grid->setMinimumHeight(125);grid->setMaximumHeight(160);layout->addWidget(grid);
    auto* controls = new QHBoxLayout;
    auto* add = new QPushButton("Добавить строку",group);add->setObjectName(name+"Add");controls->addWidget(add);
    auto* remove = new QPushButton("Удалить выбранные",group);remove->setObjectName(name+"Remove");controls->addWidget(remove);controls->addStretch();layout->addLayout(controls);
    QObject::connect(add,&QPushButton::clicked,grid,[grid,defaults] {
        const auto row = grid->rowCount();grid->insertRow(row);
        for (int column = 0; column < defaults.size(); ++column) { grid->setItem(row,column,new QTableWidgetItem(defaults[column])); }
        grid->setCurrentCell(row,0);
    });
    QObject::connect(remove,&QPushButton::clicked,grid,[grid] {
        QSet<int> selected;for (const auto* item : grid->selectedItems()) { selected.insert(item->row()); }
        for (int row = grid->rowCount()-1; row >= 0; --row) { if (selected.contains(row)) { grid->removeRow(row); } }
    });
    parent->addWidget(group);return grid;
}
void fill(QTableWidget* table,const InputRows& values) {
    table->setRowCount(static_cast<int>(values.size()));
    for (int row = 0; row < values.size(); ++row) {
        for (int column = 0; column < table->columnCount(); ++column) { table->setItem(row,column,new QTableWidgetItem(values[row].value(column))); }
    }
}
InputRows rows(const QTableWidget* table) {
    InputRows result;
    for (int row = 0; row < table->rowCount(); ++row) {
        QStringList values;for (int column = 0; column < table->columnCount(); ++column) {
            const auto* item = table->item(row,column);values.append(item ? item->text() : QString{});
        }result.append(values);
    }return result;
}
QVBoxLayout* tab(QTabWidget* tabs,const QString& title) {
    auto* scroll = new QScrollArea(tabs);scroll->setWidgetResizable(true);auto* body = new QWidget(scroll);scroll->setWidget(body);
    tabs->addTab(scroll,title);return new QVBoxLayout(body);
}
void errorStyle(QLabel* label) { label->setStyleSheet("color: #d05050;");label->hide(); }
}
ScenarioFormDialog::ScenarioFormDialog(const FormData& initial,QWidget* parent) : QDialog(parent) {
    setObjectName("scenarioFormDialog");setWindowTitle("Новый сценарий");resize(860,680);
    auto* layout = new QVBoxLayout(this);layout->addWidget(explanation("Задайте условия до расчёта. Время — целые микросекунды; тип детектора и стратегия берутся из основного окна."));
    auto* tabs = new QTabWidget(this);layout->addWidget(tabs,1);
    auto* cluster = tab(tabs,"Узлы и отказы");
    m_nodes = table(cluster,"Состав узлов","nodesTable",{"ID узла","Производительность, ед./с"},{"1","1000000"});
    cluster->addWidget(explanation("Пустой список отказов означает работу без физических отказов. Разные узлы могут отказать одновременно."));
    m_failures = table(cluster,"Физические отказы до горизонта включительно","failuresTable",{"ID узла","Момент отказа, мкс"},{"1","0"});
    const auto fields = [this](QVBoxLayout* destination,const QVector<QPair<QString,QString>>& names) {
        auto* form = new QFormLayout;
        for (const auto& pair : names) {
            auto* field = new QLineEdit(this);field->setObjectName("field_"+pair.first);m_fields.insert(pair.first,field);form->addRow(pair.second,field);
        }destination->addLayout(form);
    };
    auto* flow = tab(tabs,"Время и задачи");
    fields(flow,{{"horizon","Горизонт, мкс"},{"taskCount","Число задач"},{"firstArrival","Первое поступление, мкс"},
        {"arrivalPeriod","Интервал поступлений, мкс"},{"workMin","Минимальная трудоёмкость, ед."},{"workMax","Максимальная трудоёмкость, ед."},{"seed","Начальное значение генератора (seed, uint64)"}});
    flow->addWidget(explanation("Создаются явные задачи и трасса доставки. Для измерения пропускной способности используется весь горизонт."));flow->addStretch();
    auto* network = tab(tabs,"Heartbeat и связь");
    fields(network,{{"heartbeat","Период heartbeat, мкс"},{"delayMin","Минимальная задержка, мкс"},{"delayMax","Максимальная задержка, мкс"},{"loss","Вероятность потери (0…1)"}});
    network->addWidget(explanation("Перерывы временно блокируют доставку heartbeat указанного узла."));
    m_blackouts = table(network,"Перерывы связи","blackoutsTable",{"ID узла","Начало, мкс","Конец, мкс"},{"1","0","5"});
    auto* detector = tab(tabs,"Детекторы");
    fields(detector,{{"timeout","Начальный тайм-аут T₀, мкс"},{"kappa","Множитель подтверждения FAILED κ (>1)"},
        {"window","Ёмкость истории интервалов M"},{"minIntervals","Минимум интервалов для прогрева"},{"coefficient","Коэффициент запаса c (≥0)"}});
    detector->addWidget(explanation("Адаптивный детектор использует последние M интервалов принятых heartbeat. Коэффициент c умножает их стандартное отклонение. Формулы остаются в симуляторе."));detector->addStretch();
    m_error = explanation({});m_error->setObjectName("formError");errorStyle(m_error);layout->addWidget(m_error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,this);
    buttons->button(QDialogButtonBox::Ok)->setText("Создать и запустить");buttons->button(QDialogButtonBox::Ok)->setObjectName("createScenario");layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,this,[this] {
        try { m_config = generatorConfig(data());accept(); }
        catch (const std::exception& error) { m_error->setText(QString::fromUtf8(error.what()));m_error->show(); }
    });setData(initial);
}
FormData ScenarioFormDialog::data() const {
    FormData result;for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) { result.fields.insert(it.key(),it.value()->text()); }
    result.nodes = rows(m_nodes);result.failures = rows(m_failures);result.blackouts = rows(m_blackouts);return result;
}
void ScenarioFormDialog::setData(const FormData& data) {
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) { it.value()->setText(data.fields.value(it.key())); }
    fill(m_nodes,data.nodes);fill(m_failures,data.failures);fill(m_blackouts,data.blackouts);
}
FailureDialog::FailureDialog(const ScenarioDocument& document,QWidget* parent) : QDialog(parent),m_document(document) {
    setObjectName("failureDialog");setWindowTitle("Отказы и сохранение копии");resize(760,460);auto* layout = new QVBoxLayout(this);
    layout->addWidget(explanation(QString("Источник: %1\nГоризонт: %2 мкс. Для каждого узла допустим один отказ от 0 до горизонта включительно.").arg(document.source).arg(document.horizon)));
    layout->addWidget(explanation("Сохранение создаст новую пару JSON/CSV. Задачи, производительность узлов, параметры связи и детекторов останутся прежними."));
    m_table = table(layout,"Список физических отказов","copyFailuresTable",{"ID узла","Момент отказа, мкс"},{"1","0"});
    InputRows values;for (const auto& failure : document.failures) { values.append({QString::number(failure.node),QString::number(failure.at)}); }fill(m_table,values);
    m_error = explanation({});m_error->setObjectName("copyFormError");errorStyle(m_error);layout->addWidget(m_error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,this);buttons->button(QDialogButtonBox::Save)->setText("Сохранить копию…");
    buttons->button(QDialogButtonBox::Save)->setObjectName("saveScenarioCopy");layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,this,[this] {
        try { m_result = parseFailures(rows(m_table),m_document.nodes,m_document.horizon);accept(); }
        catch (const std::exception& error) { m_error->setText(QString::fromUtf8(error.what()));m_error->show(); }
    });
}
} // namespace telemetry::gui
