#include "run_data.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QUuid>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <cmath>

namespace telemetry::gui {
namespace {
using Json = nlohmann::basic_json<std::map,std::vector,std::string,bool,std::int64_t,std::uint64_t,long double>;
[[noreturn]] void fail(const QString& text) { throw std::runtime_error(text.toUtf8().constData()); }
void require(bool condition, const QString& text) { if (!condition) { fail(text); } }
QJsonObject object(const QByteArray& bytes, const QString& path) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && doc.isObject(),
        QString("%1: ошибка чтения JSON: %2").arg(path, error.errorString()));
    return doc.object();
}
QByteArray readBytes(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QString("Не удалось открыть %1: %2").arg(path, file.errorString()));
    const auto bytes = file.readAll();
    require(file.error() == QFileDevice::NoError, QString("Ошибка чтения %1: %2").arg(path, file.errorString()));
    return bytes;
}
QJsonObject readObject(const QString& path) { return object(readBytes(path),path); }
Json readScenario(const QString& path) {
    const auto bytes = readBytes(path);
    try {
        auto json = Json::parse(bytes.constData(),bytes.constData()+bytes.size());
        require(json.is_object(),path + ": требуется JSON-объект сценария");
        return json;
    } catch (const nlohmann::json::exception& error) {
        fail(QString("%1: ошибка чтения JSON: %2").arg(path,QString::fromUtf8(error.what())));
    }
}
QString absolute(const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
qint64 integer(const QJsonObject& row, const QString& key) {
    const auto value = row.value(key);
    const auto number = value.toInteger(-1);
    require(value.isDouble() && number >= 0, QString("Некорректное целое поле %1 в результатах").arg(key));
    return number;
}
}
QByteArray sha256(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QString("Не удалось открыть %1: %2").arg(path, file.errorString()));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file) && file.error() == QFileDevice::NoError, QString("Ошибка чтения %1").arg(path));
    return hash.result();
}
InputInfo inspectInput(const QString& path) {
    require(!path.trimmed().isEmpty() && !path.contains(QChar::Null), "Выберите JSON сценария.");
    const auto source = absolute(path);
    const auto json = readScenario(source);
    const auto trace = json.find("delivery_trace");
    require(trace != json.end() && trace->is_string(),source + ": требуется непустой путь delivery_trace");
    const auto tracePath = QString::fromStdString(trace->get<std::string>());
    require(!tracePath.isEmpty() && !tracePath.contains(QChar::Null),
        source + ": требуется непустой путь delivery_trace");
    const auto delivery = absolute(QDir(QFileInfo(source).absolutePath()).filePath(tracePath));
    QFile csv(delivery);
    require(csv.open(QIODevice::ReadOnly), QString("Не удалось открыть связанную трассу %1: %2").arg(delivery, csv.errorString()));
    qint64 horizon = -1;
    const auto at = json.find("horizon_ticks");
    if (at != json.end() && at->is_number_unsigned()) {
        if (at->get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())) { horizon = at->get<qint64>(); }
    } else if (at != json.end() && at->is_number_integer()) { horizon = at->get<qint64>(); }
    const auto count = [&json](const char* name) {
        const auto value = json.find(name);
        return value != json.end() && value->is_array() ? static_cast<qsizetype>(value->size()) : qsizetype{0};
    };
    return {source,delivery,horizon,count("nodes"),count("tasks")};
}
PreparedRun prepareRun(RunRequest request) {
    require(request.detector == "fixed" || request.detector == "adaptive", "Неизвестный детектор");
    require(request.scheduler == "rr" || request.scheduler == "ll", "Неизвестная стратегия");
    require(!request.program.trimmed().isEmpty(), "Выберите исполняемый файл telemetry_sim.");
    request.program = absolute(request.program);
    const QFileInfo program(request.program);
    require(program.isFile() && program.isExecutable(), "telemetry_sim отсутствует или не является исполняемым файлом: " + request.program);
    require(!request.outputRoot.trimmed().isEmpty(), "Выберите каталог результатов.");
    request.outputRoot = absolute(request.outputRoot);
    PreparedRun prepared;
    prepared.request = request;
    prepared.input = inspectInput(request.scenario);
    prepared.request.scenario = prepared.input.scenario;
    prepared.scenarioHash = sha256(prepared.input.scenario);
    prepared.deliveryHash = sha256(prepared.input.delivery);
    QDir root(request.outputRoot);
    require(root.mkpath("."), "Не удалось создать каталог результатов: " + root.absolutePath());
    const auto name = "run-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmsszzz'Z'-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    require(root.mkdir(name), "Не удалось выделить новый каталог прогона.");
    prepared.session = root.absoluteFilePath(name);
    prepared.directory = QDir(prepared.session).filePath("result"); // CLI must receive a fresh destination.
    QJsonObject descriptor{{"scenario",prepared.input.scenario},{"delivery",prepared.input.delivery},
        {"scenario_sha256",QString::fromLatin1(prepared.scenarioHash.toHex())},
        {"delivery_sha256",QString::fromLatin1(prepared.deliveryHash.toHex())},
        {"detector",request.detector},{"scheduler",request.scheduler},{"program",request.program},
        {"result_directory",prepared.directory}};
    QSaveFile file(QDir(prepared.session).filePath("request.json"));
    require(file.open(QIODevice::WriteOnly), "Не удалось записать описание прогона.");
    const auto data = QJsonDocument(descriptor).toJson();
    require(file.write(data) == data.size() && file.commit(), "Не удалось сохранить описание прогона.");
    return prepared;
}
void ensureUnchanged(const PreparedRun& prepared) {
    require(sha256(prepared.input.scenario) == prepared.scenarioHash
        && sha256(prepared.input.delivery) == prepared.deliveryHash,
        "Входные файлы изменились во время проверки. Повторите запуск с новыми данными.");
}
RunResult loadResult(const PreparedRun& prepared) {
    const QDir directory(prepared.directory);
    const auto manifest = readObject(directory.filePath("manifest.json"));
    require(integer(manifest,"schema_version") == 1 && manifest.value("status") == "complete",
        "manifest.json не подтверждает успешное завершение (status=complete).");
    require(manifest.value("variant").toString() == prepared.request.detector + "_" + prepared.request.scheduler,
        "Результат получен для другой пары алгоритмов.");
    require(integer(manifest.value("parameters").toObject(),"horizon_ticks") == prepared.input.horizon,
        "Горизонт результата не совпадает со сценарием.");
    require(sha256(directory.filePath("input/scenario.json")) == prepared.scenarioHash
        && sha256(directory.filePath("input/delivery.csv")) == prepared.deliveryHash,
        "Снимки результата не совпадают с входами этого запуска.");
    for (const auto* name : {"tasks.csv","attempts.csv","nodes.csv"}) {
        QFile file(directory.filePath(name));
        require(file.open(QIODevice::ReadOnly), QString("Отсутствует или недоступен %1").arg(name));
    }
    QFile journal(directory.filePath("events.jsonl"));
    require(journal.open(QIODevice::ReadOnly), "Не удалось открыть events.jsonl");
    qint64 records = 0;
    qint64 previous = 0;
    bool completed = false;
    quint64 journalChecksum = 14695981039346656037ULL;
    while (!journal.atEnd()) {
        require(!completed, "После run_completed обнаружены дополнительные записи.");
        const auto bytes = journal.readLine();
        for (const auto byte : bytes) { journalChecksum ^= static_cast<unsigned char>(byte);journalChecksum *= 1099511628211ULL; }
        const auto row = object(bytes, "events.jsonl");
        ++records;
        const auto at = integer(row,"time_ticks");
        require(integer(row,"schema_version") == 1 && integer(row,"record_no") == records
            && at >= previous && at <= prepared.input.horizon, "Нарушен порядок записей или времени events.jsonl");
        previous = at;
        if (records == 1) {
            require(row.value("kind") == "run_started" && at == 0, "Нет начальной записи run_started");
        } else if (row.value("kind") == "run_completed") {
            require(at == prepared.input.horizon && integer(row,"phase") == 0,
                "run_completed не подтверждает достижение горизонта.");
            require(integer(row,"executed_events") == integer(manifest,"executed_events"),
                "Число выполненных событий отличается от manifest.json");
            completed = true;
        }
    }
    require(journal.error() == QFileDevice::NoError && completed, "Нет завершающей записи run_completed.");
    require(manifest.value("checksum_algorithm") == "fnv1a64"
        && manifest.value("journal_checksum").toString() == QString::number(journalChecksum,16).rightJustified(16,'0'),
        "Контрольная сумма events.jsonl не совпадает с manifest.json");
    const auto metrics = readObject(directory.filePath("metrics.json"));
    require(integer(metrics,"schema_version") == 1
        && integer(metrics.value("units").toObject(),"tick_ns") == 1000, "Неизвестный формат или единицы metrics.json");
    const auto arrived = integer(metrics,"arrived");
    const auto done = integer(metrics,"completed");
    require(done <= arrived && integer(metrics,"unfinished") == arrived - done, "Нарушен баланс задач в metrics.json");
    static_cast<void>(integer(metrics,"false_positives"));
    for (const auto* name : {"mean_latency","mean_detection","mean_recovery"}) {
        const auto metric = metrics.value(name).toObject();
        const auto value = metric.value("value_ticks");
        require((value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0)
            || (value.isNull() && !metric.value("reason").toString().isEmpty()),
            QString("Некорректная метрика %1 или причина её отсутствия").arg(name));
    }
    auto bound = prepared;
    const auto playback = loadReplay(prepared.directory); // Reads the saved input with long double too.
    bound.input.nodes = playback->nodes.size();
    bound.input.tasks = playback->tasks.size();
    return {bound,metrics,records,playback};
}
QString detectorName(const QString& value) {
    return value == "fixed" ? "Фиксированный" : "Адаптивный";
}
QString schedulerName(const QString& value) {
    return value == "rr" ? "Round Robin — по кругу" : "Least Loaded — наименее загруженный";
}
QString durationText(const QJsonObject& metric) {
    if (metric.value("value_ticks").isDouble()) {
        const auto whole = metric.value("value_ticks").toInteger(-1);
        if (whole >= 0) { return QString::number(whole) + " мкс"; }
        return QString::number(metric.value("value_ticks").toDouble(),'g',12) + " мкс";
    }
    const auto reason = metric.value("reason").toString();
    const QMap<QString,QString> reasons{{"empty_sample","нет наблюдений"},{"not_detected","отказ не обнаружен до горизонта"},
        {"recovery_incomplete","восстановление не завершено"},{"not_applicable","не применимо"},
        {"crash_after_false_exclusion","отказ после ложного исключения"}};
    return reasons.value(reason,reason.isEmpty() ? "значение отсутствует" : reason) + " (" + reason + ")";
}
} // namespace telemetry::gui
