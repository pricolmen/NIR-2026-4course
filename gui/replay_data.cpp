#include "replay.hpp"
#include "run_data.hpp"
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace telemetry::gui {
namespace {
using Json = nlohmann::basic_json<std::map,std::vector,std::string,bool,std::int64_t,std::uint64_t,long double>;
QString text(const Json& value) { return QString::fromStdString(value.get<std::string>()); }
QString id(const Json& value) {
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() == 0) { throw std::runtime_error("Некорректный ID в журнале"); }
    return QString::number(value.get<std::uint64_t>());
}
QByteArray read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { throw std::runtime_error(("Не удалось прочитать " + path).toStdString()); }
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) { throw std::runtime_error("Ошибка чтения результатов"); }
    return bytes;
}
Json parse(const QByteArray& bytes) { return Json::parse(bytes.constData(),bytes.constData()+bytes.size()); }
AttemptRef attempt(const Json& value) { return {id(value.at("task_id")),id(value.at("node_id")),id(value.at("attempt_no"))}; }
QString metric(const Json& value) {
    if (value.at("value_ticks").is_null()) {
        return durationText({{"value_ticks",QJsonValue::Null},{"reason",text(value.at("reason"))}});
    }
    return QString::number(value.at("value_ticks").get<std::int64_t>()) + " мкс";
}
}
QSharedPointer<const ReplayData> loadReplay(const QString& directory) {
    auto data = QSharedPointer<ReplayData>::create();const QDir dir(directory);
    const auto input = parse(read(dir.filePath("input/scenario.json")));
    data->horizon = input.at("horizon_ticks").get<qint64>();
    for (const auto& node : input.at("nodes")) { data->nodes.append(id(node.at("id"))); }
    for (const auto& task : input.at("tasks")) { data->tasks.append(id(task.at("id"))); }
    const auto numerical = [](const QString& a, const QString& b) { return a.toULongLong() < b.toULongLong(); };
    std::sort(data->nodes.begin(),data->nodes.end(),numerical);std::sort(data->tasks.begin(),data->tasks.end(),numerical);
    QFile journal(dir.filePath("events.jsonl"));
    if (!journal.open(QIODevice::ReadOnly)) { throw std::runtime_error("Нет журнала воспроизведения"); }
    quint64 checksum = 14695981039346656037ULL;
    const QSet<QString> kinds{"run_started","run_completed","task_arrived","task_assigned","reconfiguration","result", "crash",
        "physical_assigned","physical_started","physical_finished","completion_skipped","assignment_revoked",
        "heartbeat_sent","heartbeat_delivered","send_skipped","dispatch","heartbeat_observed","timeout_observed"};
    while (!journal.atEnd()) {
        const auto bytes = journal.readLine();for (const auto byte : bytes) { checksum ^= static_cast<unsigned char>(byte);checksum *= 1099511628211ULL; }
        const auto row = parse(bytes);
        ReplayEvent event;event.record = row.at("record_no").get<qint64>();event.time = row.at("time_ticks").get<qint64>();event.kind = text(row.at("kind"));
        if (!kinds.contains(event.kind)) { throw std::runtime_error("Неизвестное событие журнала"); }
        if (event.kind == "task_arrived") { event.task = id(row.at("task").at("id")); }
        else if (event.kind == "task_assigned" || event.kind == "physical_assigned") {
            const auto& assignment = row.at("assignment");event.task = id(assignment.at("task").at("id"));
            event.node = id(assignment.at("node_id"));event.attempt = id(assignment.at("attempt_no"));
        } else if (row.contains("attempt")) {
            const auto ref = attempt(row.at("attempt"));event.task = ref.task;event.node = ref.node;event.attempt = ref.attempt;
            if (row.contains("decision")) { event.decision = text(row.at("decision")); }
            if (row.contains("reason")) { event.decision = text(row.at("reason")); }
        } else if (event.kind == "reconfiguration") {
            for (const auto& item : row.at("revoked")) { event.revoked.append(attempt(item)); }
        } else if (event.kind == "crash") {
            event.node = id(row.at("node_id"));
            if (!row.at("interrupted").is_null()) { const auto ref = attempt(row.at("interrupted"));event.task = ref.task;event.attempt = ref.attempt; }
        } else if (event.kind == "heartbeat_observed" || event.kind == "timeout_observed") {
            event.node = id(row.at(event.kind == "heartbeat_observed" ? "message" : "request").at("node_id"));
            event.before = text(row.at("before")).toUpper();event.after = text(row.at("after")).toUpper();
        }
        if (event.record != data->events.size()+1 || event.time < 0 || event.time > data->horizon
            || (!data->events.empty() && event.time < data->events.last().time)) { throw std::runtime_error("Нарушен порядок записей воспроизведения"); }
        data->events.append(event);
    }
    const auto manifest = parse(read(dir.filePath("manifest.json")));
    if (journal.error() != QFileDevice::NoError || data->events.empty() || data->events.last().kind != "run_completed"
        || data->events.last().time != data->horizon || text(manifest.at("journal_checksum")) != QString::number(checksum,16).rightJustified(16,'0')) {
        throw std::runtime_error("Журнал изменился во время загрузки воспроизведения");
    }
    const auto metrics = parse(read(dir.filePath("metrics.json")));
    for (const auto& node : metrics.at("nodes")) {
        data->metrics.append({id(node.at("node_id")),text(node.at("classification")),metric(node.at("detection")),metric(node.at("recovery"))});
    }
    return data;
}
} // namespace telemetry::gui
