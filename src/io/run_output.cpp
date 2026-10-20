#include "telemetry/io/run_output.hpp"
#include "telemetry/io/build_info.hpp"
#include "telemetry/io/journal.hpp"
#include "json_support.hpp"

#include <fstream>
#include <iomanip>
#include <iterator>

namespace telemetry::io {
namespace {
using namespace detail;
namespace fs = std::filesystem;
std::ofstream output(const fs::path& file) {
    std::ofstream stream{file,std::ios::binary}; if (!stream) { throw std::runtime_error("Cannot write "+file.string()); }
    stream.imbue(std::locale::classic()); stream << std::setprecision(std::numeric_limits<long double>::max_digits10); return stream;
}
void close(std::ofstream& stream) {
    stream.flush(); require(static_cast<bool>(stream)); stream.close(); require(!stream.fail());
}
void save(const fs::path& file,const Json& value) { auto stream = output(file); stream << value.dump(2) << '\n'; close(stream); }
Json load(const fs::path& file) {
    std::ifstream stream{file,std::ios::binary}; if (!stream) { throw std::runtime_error("Cannot read "+file.string()); }
    const std::string content{std::istreambuf_iterator<char>{stream},{}}; require(!stream.bad()); return parse(content);
}
// Stable FNV-1a-64 over file bytes. Integrity metadata, not a security primitive.
std::string checksum(const fs::path& file) {
    std::ifstream stream{file,std::ios::binary}; if (!stream) { throw std::runtime_error("Cannot checksum "+file.string()); }
    std::uint64_t hash = 14695981039346656037ULL; std::array<char,8192> buffer{};
    while (stream) {
        stream.read(buffer.data(),static_cast<std::streamsize>(buffer.size()));
        for (std::streamsize i = 0; i < stream.gcount(); ++i) { hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]); hash *= 1099511628211ULL; }
    }
    require(stream.eof() && !stream.bad()); std::ostringstream text; text << std::hex << std::setfill('0') << std::setw(16) << hash; return text.str();
}
Json metric(const metrics::DurationMetric& value) { return {{"value_ticks",value.value ? Json(value.value->ticks()) : Json(nullptr)},{"reason",value.value ? Json(nullptr) : Json(value.reason)}}; }
Json mean(std::optional<long double> value,const char* reason = "empty_sample") {
    return {{"value_ticks",value ? Json(static_cast<double>(*value)) : Json(nullptr)},{"reason",value ? Json(nullptr) : Json(reason)}};
}
std::string cell(std::optional<SimTime> value) { return value ? std::to_string(value->ticks()) : ""; }
std::string cell(const metrics::DurationMetric& value) { return value.value ? std::to_string(value.value->ticks()) : ""; }
void reports(const fs::path& dir,const metrics::Analysis& report) {
    const auto& s = report.summary;
    Json windows = Json::array();
    for (const auto& row : s.throughput) { windows.push_back({{"start_ticks",row.window.start.ticks()},{"end_ticks",row.window.end.ticks()},
        {"completed",row.completed},{"tasks_per_second",static_cast<double>(row.tasks_per_second)}}); }
    Json nodes = Json::array();
    for (const auto& [id,row] : report.nodes) {
        nodes.push_back({{"node_id",id.value()},{"classification",row.classification},{"detection",metric(row.detection)},{"recovery",metric(row.recovery)}});
    }
    Json json{{"schema_version",1},{"units",{{"tick_ns",1000},{"durations","ticks"},{"throughput","tasks/second"}}},
        {"arrived",s.arrived},{"completed",s.completed},{"unfinished",s.unfinished},{"queued",s.queued},{"assigned",s.assigned},
        {"retries",s.retries},{"rejected_revoked_results",s.rejected_revoked_results},{"crashes",s.crashes},{"false_positives",s.false_positives},{"false_negatives",s.false_negatives},
        {"crashes_after_false_exclusion",s.crashes_after_false_exclusion},{"latency_samples",s.completed},{"detection_samples",s.detection_samples},{"recovery_samples",s.recovery_samples},
        {"incomplete_recoveries",s.incomplete_recoveries},{"mean_latency",mean(s.mean_latency_ticks)},{"p95_latency",metric({s.p95_latency,s.p95_latency ? "" : "empty_sample"})},
        {"mean_detection",mean(s.mean_detection_ticks)},{"mean_recovery",mean(s.mean_recovery_ticks)},{"throughput_windows",windows},{"nodes",nodes},
        {"invariants",{{"balance",true},{"unique_tasks",true},{"unique_acceptance",true},{"logical_physical_attempts",true}}}};
    auto tasks = output(dir/"tasks.csv"); tasks << "task_id,arrival_ticks,work,accepted_ticks,latency_ticks,attempts,unfinished,latency_reason\n";
    for (const auto& [id,row] : report.tasks) {
        tasks << id.value() << ',' << row.description.arrival.ticks() << ',' << row.description.work << ',' << cell(row.completed_at) << ',';
        if (row.completed_at) { tasks << (*row.completed_at-row.description.arrival).ticks(); }
        tasks << ',' << row.attempts << ',' << (row.completed_at ? 0 : 1) << ',' << (row.completed_at ? "" : "unfinished") << '\n';
    }
    close(tasks);
    auto attempts = output(dir/"attempts.csv"); attempts << "task_id,attempt_no,node_id,assigned_ticks,started_ticks,finished_ticks,interrupted_ticks,revoked_ticks,accepted_ticks,result_decision\n";
    for (const auto& [key,row] : report.attempts) {
        attempts << key.task.value() << ',' << key.attempt.value() << ',' << row.assignment.node.value() << ',' << row.assigned_at.ticks() << ','
            << cell(row.started_at) << ',' << cell(row.finished_at) << ',' << cell(row.interrupted_at) << ',' << cell(row.revoked_at) << ',' << cell(row.accepted_at) << ','
            << (row.result ? name(*row.result,result_decisions) : "") << '\n';
    }
    close(attempts);
    auto node_csv = output(dir/"nodes.csv"); node_csv << "node_id,crash_ticks,failed_ticks,final_status,classification,returned_tasks,detection_ticks,detection_reason,recovery_ticks,recovery_reason\n";
    for (const auto& [id,row] : report.nodes) {
        node_csv << id.value() << ',' << cell(row.crash_at) << ',' << cell(row.failed_at) << ',' << name(row.status,statuses) << ',' << row.classification << ',';
        for (std::size_t i = 0; i < row.returned.size(); ++i) { if (i) { node_csv << ';'; } node_csv << row.returned[i].value(); }
        node_csv << ',' << cell(row.detection) << ',' << row.detection.reason << ',' << cell(row.recovery) << ',' << row.recovery.reason << '\n';
    }
    close(node_csv); save(dir/"metrics.json",json);
}
Json parameters(const simulation::Scenario& scenario) {
    Json nodes = Json::array(); for (auto node : scenario.physical()->nodes()) { nodes.push_back({{"id",node.id.value()},{"performance_decimal",decimal(node.performance)}}); }
    return {{"tick_ns",1000},{"horizon_ticks",scenario.physical()->horizon().ticks()},{"heartbeat_period_ticks",scenario.timeout().heartbeat_period.ticks()},
        {"tau0_ticks",scenario.timeout().initial_timeout.ticks()},{"kappa_decimal",decimal(scenario.timeout().kappa)},
        {"adaptive",{{"window",scenario.adaptive().window_size},{"min_intervals",scenario.adaptive().min_intervals},{"c_decimal",decimal(scenario.adaptive().coefficient)}}},{"nodes",nodes}};
}
Json environment() {
    Json value = parse(build_info_json());
#if defined(__APPLE__)
    value["platform"] = "macOS";
#elif defined(__linux__)
    value["platform"] = "Linux";
#elif defined(_WIN32)
    value["platform"] = "Windows";
#else
    value["platform"] = "unknown";
#endif
    value["pointer_bits"] = sizeof(void*)*8; value["long_double_digits"] = std::numeric_limits<long double>::digits;
    value["numerical_rules"] = "tick=1us;processing=max(1,ceil(w/p*1e6));S=ceil(rawS);F=max(S+1,ceil(rawF));p95=n-n/20"; return value;
}
struct Sink : simulation::SimulationAuditSink {
    JournalWriter& writer; metrics::Analyzer& analyzer;
    Sink(JournalWriter& w,metrics::Analyzer& a) : writer(w),analyzer(a) {}
    void on_event(const engine::EventKey& key,const simulation::SimulationEvent& event) override { analyzer.on_event(key,event); writer.on_event(key,event); }
};
}
metrics::Analysis write_run(const fs::path& scenario_file,simulation::Variant variant,const fs::path& dir) {
    // Validate all read-only prerequisites before creating output files.
    const auto loaded = load_scenario(scenario_file); const auto variant_name = name(variant,variants);
    const auto json_hash = checksum(loaded.scenario_file),trace_hash = checksum(loaded.delivery_file);
    if (fs::exists(dir)) {
        if (!fs::is_directory(dir) || !fs::is_empty(dir)) { throw std::runtime_error("Output directory must be empty: "+dir.string()); }
    } else { fs::create_directories(dir); }
    Json manifest{{"schema_version",1},{"status","incomplete"},{"variant",variant_name},{"environment",environment()},
        {"parameters",parameters(*loaded.scenario)},{"checksum_algorithm","fnv1a64"},
        {"inputs",{{"scenario",{{"original_path",loaded.scenario_file.string()},{"snapshot","input/scenario.json"},{"checksum",json_hash}}},
                    {"delivery",{{"original_path",loaded.delivery_file.string()},{"snapshot","input/delivery.csv"},{"checksum",trace_hash}}}}}};
    save(dir/"manifest.json",manifest);
    fs::create_directory(dir/"input"); fs::copy_file(loaded.scenario_file,dir/"input/scenario.json"); fs::copy_file(loaded.delivery_file,dir/"input/delivery.csv");
    require(checksum(loaded.scenario_file) == json_hash && checksum(loaded.delivery_file) == trace_hash &&
        checksum(dir/"input/scenario.json") == json_hash && checksum(dir/"input/delivery.csv") == trace_hash);
    const auto snapshot = load_scenario(dir/"input/scenario.json",dir/"input/delivery.csv");
    manifest["parameters"] = parameters(*snapshot.scenario);
    auto journal = output(dir/"events.jsonl"); JournalWriter writer{journal}; metrics::Analyzer analyzer{snapshot.scenario}; Sink sink{writer,analyzer};
    simulation::Simulation run{snapshot.scenario,variant,&sink}; const auto result = run.run(); auto analysis = analyzer.finish(result);
    writer.complete(result); close(journal); reports(dir,analysis);
    manifest["journal_checksum"] = checksum(dir/"events.jsonl"); manifest["status"] = "complete"; manifest["executed_events"] = result.executed;
    save(dir/"manifest.json",manifest); return analysis;
}
metrics::Analysis analyze_run(const fs::path& dir) {
    const auto manifest = load(dir/"manifest.json");
    require(integer(manifest.at("schema_version")) == 1 && manifest.at("status") == "complete" && manifest.at("checksum_algorithm") == "fnv1a64");
    static_cast<void>(enumeration<simulation::Variant>(manifest.at("variant"),variants));
    require(manifest.at("inputs").at("scenario").at("snapshot") == "input/scenario.json" && manifest.at("inputs").at("delivery").at("snapshot") == "input/delivery.csv");
    require(manifest.at("inputs").at("scenario").at("checksum") == checksum(dir/"input/scenario.json") &&
        manifest.at("inputs").at("delivery").at("checksum") == checksum(dir/"input/delivery.csv") &&
        manifest.at("journal_checksum") == checksum(dir/"events.jsonl"));
    const auto loaded = load_scenario(dir/"input/scenario.json",dir/"input/delivery.csv"); require(manifest.at("parameters") == parameters(*loaded.scenario));
    metrics::Analyzer analyzer{loaded.scenario}; std::ifstream journal{dir/"events.jsonl",std::ios::binary}; require(static_cast<bool>(journal));
    const auto result = read_journal(journal,analyzer); require(result.executed == integer(manifest.at("executed_events"))); return analyzer.finish(result);
}
void recalculate_reports(const fs::path& dir) { const auto analysis = analyze_run(dir); reports(dir,analysis); }
}  // namespace telemetry::io
