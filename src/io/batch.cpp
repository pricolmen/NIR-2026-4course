#include "telemetry/io/experiments.hpp"
#include "comparison_support.hpp"
#include <set>
#include <tuple>

namespace telemetry::io {
namespace {
using namespace detail;
void fields(const Json& value,std::initializer_list<const char*> names) {
    require(value.is_object() && value.size() == names.size()); for (auto key : names) { require(value.contains(key)); }
}
std::string label(const Json& value) {
    require(value.is_string()); const auto text = value.get<std::string>(); require(!text.empty() && text.find('\0') == std::string::npos); return text;
}
struct Average {
    std::uint64_t count = 0; long double mean = 0;
    void add(std::optional<long double> value) {
        if (value) { count = checked_increment(count); mean += (*value-mean)/static_cast<long double>(count); require(std::isfinite(mean)); }
    }
    void cells(std::vector<std::string>& row) const {
        row.insert(row.end(),{count ? decimal(mean) : "",std::to_string(count),count ? "" : "empty_sample"});
    }
};
void sum(std::uint64_t& target,std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max()-target) { throw std::overflow_error("Batch count overflow"); } target += value;
}
struct Aggregate {
    std::uint64_t runs = 0,detection_observations = 0,recovery_observations = 0,fp = 0,fn = 0,late = 0,incomplete = 0,unfinished = 0;
    Average latency,p95,detection,recovery,throughput;
};
using Key = std::tuple<std::string,std::size_t,std::optional<std::size_t>,SimTime,SimTime>;
}
std::vector<BatchEntry> load_batch(const fs::path& requested) {
    const auto file = fs::absolute(requested).lexically_normal();
    try {
    const auto json = load(file); fields(json,{"schema_version","runs"});
    require(integer(json.at("schema_version")) == 1 && json.at("runs").is_array() && !json.at("runs").empty());
    std::set<std::pair<std::string,std::string>> pairs; std::vector<BatchEntry> entries;
    for (const auto& row : json.at("runs")) {
        fields(row,{"scenario_id","repeat_id","scenario"}); const auto id = label(row.at("scenario_id")),repeat = label(row.at("repeat_id"));
        require(pairs.emplace(id,repeat).second); const auto path = label(row.at("scenario")); const auto loaded = load_scenario(file.parent_path()/fs::path{path});
        entries.push_back({id,repeat,loaded.scenario_file,loaded.delivery_file});
    }
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b) { return std::tie(a.scenario_id,a.repeat_id) < std::tie(b.scenario_id,b.repeat_id); });
    return entries;
    } catch (const InputError&) { throw; }
    catch (const std::exception& error) { throw InputError(file,"$",error.what()); }
}
std::vector<BatchReport> write_batch(const std::vector<BatchEntry>& entries,const fs::path& dir) {
    require(!entries.empty() && (!fs::exists(dir) || (fs::is_directory(dir) && fs::is_empty(dir)))); fs::create_directories(dir);
    Json manifest{{"schema_version",1},{"status","incomplete"},{"runs",Json::array()}}; save(dir/"batch_manifest.json",manifest);
    auto comparison = output(dir/"comparison.csv"); comparison_header(comparison); std::map<Key,Aggregate> aggregates;
    std::vector<BatchReport> completed;
    std::uint64_t ordinal = 0;
    for (const auto& entry : entries) {
        ordinal = checked_increment(ordinal); std::ostringstream number; number << std::setfill('0') << std::setw(6) << ordinal;
        const auto relative = fs::path{"runs"}/number.str(); const auto reports = write_comparison(entry.scenario_file,dir/relative);
        completed.push_back({entry,relative,reports});
        manifest["runs"].push_back({{"scenario_id",entry.scenario_id},{"repeat_id",entry.repeat_id},{"scenario",entry.scenario_file.string()},{"output",relative.generic_string()}});
        for (std::size_t i = 0; i < reports.size(); ++i) {
            const auto& s = reports[i]; comparison_rows(comparison,s,variants[i],entry.scenario_id,entry.repeat_id);
            for (std::size_t w = 0; w < std::max(std::size_t{1},s.throughput.size()); ++w) {
                const bool has_window = !s.throughput.empty();
                Key key{entry.scenario_id,i,has_window ? std::optional<std::size_t>{w} : std::nullopt,
                    has_window ? s.throughput[w].window.start : SimTime{},has_window ? s.throughput[w].window.end : SimTime{}};
                auto& a = aggregates[key]; a.runs = checked_increment(a.runs);
                sum(a.detection_observations,s.detection_samples); sum(a.recovery_observations,s.recovery_samples);
                sum(a.fp,s.false_positives); sum(a.fn,s.false_negatives); sum(a.late,s.crashes_after_false_exclusion); sum(a.incomplete,s.incomplete_recoveries); sum(a.unfinished,s.unfinished);
                a.latency.add(s.mean_latency_ticks); a.p95.add(s.p95_latency ? std::optional<long double>{static_cast<long double>(s.p95_latency->ticks())} : std::nullopt);
                a.detection.add(s.mean_detection_ticks); a.recovery.add(s.mean_recovery_ticks);
                a.throughput.add(has_window ? std::optional<long double>{s.throughput[w].tasks_per_second} : std::nullopt);
            }
        }
    }
    close(comparison); auto summary = output(dir/"summary.csv");
    csv(summary,{"scenario_id","variant","window_index","window_start_ticks","window_end_ticks","runs",
        "mean_run_latency_ticks","latency_run_samples","latency_reason","mean_run_p95_ticks","p95_run_samples","p95_reason",
        "mean_run_detection_ticks","detection_run_samples","detection_reason","mean_run_recovery_ticks","recovery_run_samples","recovery_reason",
        "mean_run_throughput_tasks_per_second","throughput_run_samples","throughput_reason","detection_node_observations","recovery_node_observations",
        "false_positives_total","false_negatives_total","crashes_after_false_exclusion_total","incomplete_recoveries_total","unfinished_total"});
    for (const auto& [key,a] : aggregates) {
        const auto& [id,variant,window,start,end] = key;
        std::vector<std::string> row{id,variants[variant],window ? std::to_string(*window) : "",window ? std::to_string(start.ticks()) : "",window ? std::to_string(end.ticks()) : "",std::to_string(a.runs)};
        for (const auto* average : {&a.latency,&a.p95,&a.detection,&a.recovery,&a.throughput}) { average->cells(row); }
        for (auto count : {a.detection_observations,a.recovery_observations,a.fp,a.fn,a.late,a.incomplete,a.unfinished}) { row.push_back(std::to_string(count)); }
        csv(summary,row);
    }
    close(summary); manifest["status"] = "complete"; save(dir/"batch_manifest.json",manifest);
    return completed;
}
}  // namespace telemetry::io
