#include "scenario_files.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace telemetry::gui {
namespace {
using Json = nlohmann::basic_json<std::map,std::vector,std::string,bool,std::int64_t,std::uint64_t,long double>;
void require(bool condition,const QString& message) { if (!condition) { throw std::runtime_error(message.toUtf8().constData()); } }
QByteArray bytes(const QString& path) {
    QFile file(path);require(file.open(QIODevice::ReadOnly),"Не удалось открыть " + path);
    const auto result = file.readAll();require(file.error() == QFileDevice::NoError,"Ошибка чтения " + path);return result;
}
Json parse(const QByteArray& value) { return Json::parse(value.constData(),value.constData()+value.size()); }
void write(const QString& path,const QByteArray& value) {
    QSaveFile file(path);require(file.open(QIODevice::WriteOnly),"Не удалось записать " + path);
    require(file.write(value) == value.size() && file.commit(),"Ошибка сохранения " + path);
}
quint64 natural(QString value,const QString& label,bool positive = false) {
    value = value.trimmed();bool valid = !value.isEmpty();
    for (const auto c : value) { valid = valid && c >= '0' && c <= '9'; }
    bool ok = false;const auto number = value.toULongLong(&ok);
    require(valid && ok && (!positive || number > 0),label + ": требуется " + (positive ? "положительное" : "неотрицательное") + " целое uint64");return number;
}
qint64 ticks(const QString& value,const QString& label,bool positive = false) {
    const auto number = natural(value,label,positive);
    require(number <= static_cast<quint64>(std::numeric_limits<qint64>::max()),label + ": превышен диапазон времени int64");
    return static_cast<qint64>(number);
}
QByteArray realLiteral(const QString& value,const QString& label,bool positive = false) {
    const auto literal = value.trimmed().toUtf8();
    try {
        const auto json = parse(literal);require(json.is_number(),label + ": требуется число");
        const auto number = json.get<long double>();
        require(std::isfinite(number) && (positive ? number > 0 : number >= 0),label + ": недопустимое значение");
    } catch (const nlohmann::json::exception&) { throw std::runtime_error((label + ": неверная запись числа (например, 1000000 или 1e6)").toUtf8().constData()); }
    return literal;
}
QByteArray failureArray(const QVector<FailureSpec>& failures) {
    QByteArray result = "[";
    for (const auto& failure : failures) {
        if (result.size() > 1) { result += ','; }
        result += "{\"node_id\":" + QByteArray::number(failure.node) + ",\"at_ticks\":" + QByteArray::number(failure.at) + '}';
    }
    return result + ']';
}
void validateFailures(const QVector<FailureSpec>& failures,const QSet<quint64>& nodes,qint64 horizon) {
    QSet<quint64> seen;
    for (const auto& failure : failures) {
        require(nodes.contains(failure.node),"Отказ: узел " + QString::number(failure.node) + " отсутствует в сценарии");
        require(!seen.contains(failure.node),"Отказ узла " + QString::number(failure.node) + " задан повторно");seen.insert(failure.node);
        require(failure.at >= 0 && failure.at <= horizon,"Момент отказа должен быть от 0 до горизонта включительно");
    }
}
PreparedRun workspace(RunRequest request,const QString& prefix) {
    require(!request.program.trimmed().isEmpty(),"Выберите telemetry_sim");
    request.program = QFileInfo(request.program).absoluteFilePath();
    require(QFileInfo(request.program).isFile() && QFileInfo(request.program).isExecutable(),"telemetry_sim отсутствует или не исполняемый");
    require(!request.outputRoot.trimmed().isEmpty(),"Выберите каталог данных");
    QDir root(QFileInfo(request.outputRoot).absoluteFilePath());require(root.mkpath("."),"Не удалось создать каталог данных");
    const auto name = prefix + '-' + QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmsszzz'Z'-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    require(root.mkdir(name),"Не удалось выделить новый каталог операции");
    PreparedRun prepared;prepared.request = request;prepared.session = root.filePath(name);return prepared;
}
void unchanged(const ScenarioDocument& document) {
    require(sha256(document.source) == document.sourceHash && sha256(document.delivery) == document.deliveryHash,
        "Исходный сценарий или трасса изменились. Откройте сценарий заново перед сохранением копии.");
}
// Locate top-level value spans only. JSON syntax and decoded keys come from the
// library; this scanner does not validate or reserialize the scenario's values.
QMap<QString,QPair<qsizetype,qsizetype>> ranges(const QByteArray& data) {
    QMap<QString,QPair<qsizetype,qsizetype>> result;
    const auto white = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
    const auto stringEnd = [&data](qsizetype start) {
        auto p = start+1;
        while (p < data.size()) { if (data[p] == '\\') { p += 2; } else if (data[p++] == '"') { return p; } }
        throw std::runtime_error("Незакрытая строка JSON");
    };
    qsizetype p = data.startsWith("\xEF\xBB\xBF") ? 3 : 0;
    while (p < data.size() && white(data[p])) { ++p; }require(p < data.size() && data[p++] == '{',"Требуется JSON-объект");
    for (;;) {
        while (p < data.size() && (white(data[p]) || data[p] == ',')) { ++p; }
        if (p >= data.size() || data[p] == '}') { break; }
        const auto keyStart = p;const auto keyEnd = stringEnd(p);
        const auto key = QString::fromStdString(parse(data.mid(keyStart,keyEnd-keyStart)).get<std::string>());
        require(!result.contains(key),"Повторяющееся поле верхнего уровня: " + key);
        p = keyEnd;while (white(data[p])) { ++p; }++p;while (white(data[p])) { ++p; }
        const auto start = p;
        if (data[p] == '"') { p = stringEnd(p); }
        else if (data[p] == '[' || data[p] == '{') {
            int depth = 0;
            do {
                if (data[p] == '"') { p = stringEnd(p);continue; }
                if (data[p] == '[' || data[p] == '{') { ++depth; }
                if (data[p] == ']' || data[p] == '}') { --depth; }++p;
            } while (depth > 0 && p < data.size());
        } else { while (p < data.size() && data[p] != ',' && data[p] != '}') { ++p; } }
        auto end = p;while (end > start && white(data[end-1])) { --end; }
        result.insert(key,{start,end});
    }
    return result;
}
}
FormData defaultForm() {
    return {{{"horizon","100"},{"heartbeat","5"},{"timeout","10"},{"kappa","2"},{"window","3"},{"minIntervals","2"},{"coefficient","2"},
        {"taskCount","4"},{"firstArrival","0"},{"arrivalPeriod","5"},{"workMin","4"},{"workMax","8"},{"delayMin","0"},{"delayMax","0"},{"loss","0"},{"seed","42"}},
        {{"1","1000000"},{"2","1000000"}},{},{}};
}
QVector<FailureSpec> parseFailures(const InputRows& rows,const QSet<quint64>& nodes,qint64 horizon) {
    QVector<FailureSpec> failures;
    for (const auto& row : rows) { require(row.size() == 2,"В строке отказа нужны узел и момент");failures.append({natural(row[0],"Узел отказа",true),ticks(row[1],"Момент отказа")}); }
    validateFailures(failures,nodes,horizon);return failures;
}
QByteArray generatorConfig(const FormData& data) {
    const auto number = [&](const char* key,bool positive = false) { return ticks(data.fields.value(key),key,positive); };
    const auto real = [&](const char* key,bool positive = false) { return realLiteral(data.fields.value(key),key,positive); };
    const auto horizon = number("horizon",true),heartbeat = number("heartbeat",true),timeout = number("timeout",true);
    const auto window = natural(data.fields.value("window"),"Окно M"),minimum = natural(data.fields.value("minIntervals"),"Минимум интервалов");
    require(timeout >= heartbeat,"Начальный тайм-аут должен быть не меньше периода heartbeat");
    require(minimum >= 2 && window >= minimum,"Требуется M ≥ минимум интервалов ≥ 2");
    const auto kappa = real("kappa",true),coefficient = real("coefficient");require(parse(kappa).get<long double>() > 1,"Множитель κ должен быть больше 1");
    QSet<quint64> ids;QByteArray nodes = "[";require(!data.nodes.empty(),"Добавьте хотя бы один узел");
    for (const auto& row : data.nodes) {
        require(row.size() == 2,"В строке узла нужны ID и производительность");const auto id = natural(row[0],"ID узла",true);
        require(!ids.contains(id),"ID узлов должны быть уникальны");ids.insert(id);
        if (nodes.size() > 1) { nodes += ','; }
        nodes += "{\"id\":" + QByteArray::number(id) + ",\"performance_work_per_second\":" + realLiteral(row[1],"Производительность",true) + '}';
    }nodes += ']';
    const auto failures = parseFailures(data.failures,ids,horizon);QByteArray blackouts = "[";
    for (const auto& row : data.blackouts) {
        require(row.size() == 3,"В перерыве нужны узел, начало и конец");const auto node = natural(row[0],"Узел перерыва",true);
        const auto start = ticks(row[1],"Начало перерыва"),end = ticks(row[2],"Конец перерыва");
        require(ids.contains(node),"Перерыв: неизвестный узел");require(start < end,"Начало перерыва должно быть раньше конца");
        if (blackouts.size() > 1) { blackouts += ','; }
        blackouts += "{\"node_id\":"+QByteArray::number(node)+",\"start_ticks\":"+QByteArray::number(start)+",\"end_ticks\":"+QByteArray::number(end)+'}';
    }blackouts += ']';
    const auto first = number("firstArrival"),period = number("arrivalPeriod",true),delayMin = number("delayMin"),delayMax = number("delayMax");
    const auto workMin = real("workMin",true),workMax = real("workMax",true),loss = real("loss");
    require(delayMin <= delayMax && parse(workMin).get<long double>() <= parse(workMax).get<long double>(),"Нижняя граница диапазона больше верхней");
    require(parse(loss).get<long double>() <= 1,"Вероятность потери должна лежать от 0 до 1");
    const auto seed = natural(data.fields.value("seed"),"Seed"),count = natural(data.fields.value("taskCount"),"Число задач");
    return "{\"schema_version\":1,\"seed\":"+QByteArray::number(seed)+",\"scenario\":{\"tick_ns\":1000,\"horizon_ticks\":"+QByteArray::number(horizon)
        +",\"heartbeat_period_ticks\":"+QByteArray::number(heartbeat)+",\"tau0_ticks\":"+QByteArray::number(timeout)+",\"kappa\":"+kappa
        +",\"adaptive\":{\"window\":"+QByteArray::number(window)+",\"min_intervals\":"+QByteArray::number(minimum)+",\"c\":"+coefficient+"},\"nodes\":"+nodes
        +",\"crashes\":"+failureArray(failures)+",\"blackouts\":"+blackouts+",\"measurement_windows\":[{\"start_ticks\":0,\"end_ticks\":"+QByteArray::number(horizon)
        +"}]},\"tasks\":{\"count\":"+QByteArray::number(count)+",\"first_arrival_ticks\":"+QByteArray::number(first)+",\"period_ticks\":"+QByteArray::number(period)
        +",\"work_min\":"+workMin+",\"work_max\":"+workMax+"},\"delivery\":{\"min_delay_ticks\":"+QByteArray::number(delayMin)+",\"max_delay_ticks\":"+QByteArray::number(delayMax)+",\"loss_probability\":"+loss+"}}\n";
}
ScenarioDocument readScenarioDocument(const QString& path) {
    ScenarioDocument doc;doc.source = QFileInfo(path).absoluteFilePath();doc.bytes = bytes(doc.source);const auto json = parse(doc.bytes);
    require(json.is_object(),"Требуется сценарий JSON");doc.horizon = json.at("horizon_ticks").get<qint64>();
    doc.delivery = QDir(QFileInfo(doc.source).absolutePath()).absoluteFilePath(QString::fromStdString(json.at("delivery_trace").get<std::string>()));
    doc.sourceHash = QCryptographicHash::hash(doc.bytes,QCryptographicHash::Sha256);doc.deliveryHash = sha256(doc.delivery);
    for (const auto& node : json.at("nodes")) { doc.nodes.insert(node.at("id").get<quint64>()); }
    for (const auto& failure : json.at("crashes")) { doc.failures.append({failure.at("node_id").get<quint64>(),failure.at("at_ticks").get<qint64>()}); }
    return doc;
}
QByteArray editedScenario(const ScenarioDocument& doc,const QVector<FailureSpec>& failures,const QString& traceName) {
    validateFailures(failures,doc.nodes,doc.horizon);static_cast<void>(parse(doc.bytes));const auto fields = ranges(doc.bytes);
    require(fields.contains("crashes") && fields.contains("delivery_trace"),"Нет полей crashes/delivery_trace");
    struct Patch { qsizetype start,end;QByteArray bytes; };QVector<Patch> patches;
    for (const auto& name : {QString("crashes"),QString("delivery_trace")}) {
        const auto range = fields[name];const auto value = name == "crashes" ? failureArray(failures) : QByteArray::fromStdString(Json(traceName.toStdString()).dump());
        patches.append({range.first,range.second,value});
    }
    std::sort(patches.begin(),patches.end(),[](const auto& a,const auto& b) { return a.start > b.start; });
    auto result = doc.bytes;for (const auto& patch : patches) { result.replace(patch.start,patch.end-patch.start,patch.bytes); }return result;
}
PreparedRun generationWorkspace(const RunRequest& request,const QByteArray& config) {
    auto prepared = workspace(request,"generation");write(QDir(prepared.session).filePath("generator_config.json"),config);return prepared;
}
PreparedRun generatedRun(const PreparedRun& generation) {
    const QDir directory(QDir(generation.session).filePath("generated"));const auto manifest = parse(bytes(directory.filePath("generation_manifest.json")));
    require(manifest.at("status") == "complete","Генератор не подтвердил complete");
    auto request = generation.request;request.scenario = directory.filePath("scenario.json");return prepareRun(request);
}
QSharedPointer<CopyWorkspace> prepareCopy(const RunRequest& request,const ScenarioDocument& doc,const QVector<FailureSpec>& failures,const QString& destination) {
    unchanged(doc);validateFailures(failures,doc.nodes,doc.horizon);require(!destination.trimmed().isEmpty(),"Укажите имя копии");
    const QFileInfo target(destination);require(!target.exists() && !target.isSymLink(),"Копия уже существует. Выберите новое имя; исходники не перезаписываются.");
    require(QFileInfo(target.absolutePath()).isDir(),"Каталог копии не существует");
    auto copy = QSharedPointer<CopyWorkspace>::create();copy->document = doc;copy->destination = target.absoluteFilePath();copy->process = workspace(request,"copy");
    copy->staging = QSharedPointer<QTemporaryDir>::create(QDir(copy->process.session).filePath("staging-XXXXXX"));require(copy->staging->isValid(),"Не удалось создать временный каталог копии");
    copy->csvName = "delivery-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".csv";
    require(QFile::copy(doc.delivery,copy->staging->filePath(copy->csvName)),"Не удалось скопировать трассу CSV");
    require(sha256(copy->staging->filePath(copy->csvName)) == doc.deliveryHash,"CSV изменился при копировании");
    copy->json = copy->staging->filePath("scenario.json");write(copy->json,editedScenario(doc,failures,copy->csvName));copy->jsonHash = sha256(copy->json);unchanged(doc);return copy;
}
QString publishCopy(const QSharedPointer<CopyWorkspace>& copy) {
    unchanged(copy->document);require(sha256(copy->json) == copy->jsonHash && sha256(copy->staging->filePath(copy->csvName)) == copy->document.deliveryHash,"Подготовленная копия изменилась после проверки");
    const QFileInfo target(copy->destination);require(!target.exists() && !target.isSymLink(),"Файл назначения уже существует");
    const auto csv = QDir(target.absolutePath()).filePath(copy->csvName);
    require(QFile::copy(copy->staging->filePath(copy->csvName),csv),"Не удалось сохранить трассу копии");
    try {
        require(sha256(csv) == copy->document.deliveryHash && QFile::rename(copy->json,copy->destination),
            "Не удалось опубликовать копию; выберите другое имя или проверьте доступ к каталогу");
    } catch (...) { QFile::remove(csv);throw; }
    return copy->destination;
}
} // namespace telemetry::gui
