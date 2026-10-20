#pragma once
#include "file_support.hpp"
#include "telemetry/metrics/analysis.hpp"

namespace telemetry::io::detail {
inline void csv(std::ostream& stream,const std::vector<std::string>& row) {
    bool first = true;
    for (const auto& field : row) {
        if (!first) { stream << ','; } first = false;
        stream << '"'; for (char c : field) { if (c == '"') { stream << '"'; } stream << c; } stream << '"';
    }
    stream << '\n'; if (!stream) { throw std::runtime_error("CSV write failed"); }
}
inline std::string numeric(std::optional<long double> value) { return value ? decimal(*value) : ""; }
inline std::string numeric(std::optional<SimDuration> value) { return value ? std::to_string(value->ticks()) : ""; }
inline void comparison_header(std::ostream& stream) {
    csv(stream,{"scenario_id","repeat_id","variant","arrived","completed","unfinished","queued","assigned","retries","rejected_revoked_results",
        "crashes","false_positives","false_negatives","crashes_after_false_exclusion","incomplete_recoveries","latency_samples","detection_samples","recovery_samples",
        "mean_latency_ticks","latency_reason","p95_latency_ticks","p95_reason","mean_detection_ticks","detection_reason","mean_recovery_ticks","recovery_reason",
        "window_index","window_start_ticks","window_end_ticks","window_completed","throughput_tasks_per_second","throughput_reason"});
}
inline void comparison_rows(std::ostream& stream,const metrics::Summary& s,const std::string& variant,
                            const std::string& scenario_id = "",const std::string& repeat_id = "") {
    for (std::size_t i = 0; i < std::max(std::size_t{1},s.throughput.size()); ++i) {
        std::vector<std::string> row{scenario_id,repeat_id,variant};
        for (auto count : {s.arrived,s.completed,s.unfinished,s.queued,s.assigned,s.retries,s.rejected_revoked_results,s.crashes,s.false_positives,
                           s.false_negatives,s.crashes_after_false_exclusion,s.incomplete_recoveries,s.completed,s.detection_samples,s.recovery_samples}) { row.push_back(std::to_string(count)); }
        const auto add = [&](auto value) { row.push_back(numeric(value)); row.push_back(value ? "" : "empty_sample"); };
        add(s.mean_latency_ticks); add(s.p95_latency); add(s.mean_detection_ticks); add(s.mean_recovery_ticks);
        if (s.throughput.empty()) { row.insert(row.end(),{"","","","","","no_measurement_window"}); }
        else {
            const auto& w = s.throughput[i]; row.insert(row.end(),{std::to_string(i),std::to_string(w.window.start.ticks()),std::to_string(w.window.end.ticks()),
                std::to_string(w.completed),decimal(w.tasks_per_second),""});
        }
        csv(stream,row);
    }
}
}  // namespace telemetry::io::detail
