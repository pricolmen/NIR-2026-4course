#include "telemetry/io/scenario_loader.hpp"

#include "scenario_json.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace telemetry::io {
namespace {
using namespace input_detail;
std::string_view trim(std::string_view text) {
    const auto start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos) { return {}; }
    const auto end = text.find_last_not_of(" \t");
    return text.substr(start,end-start+1);
}
std::array<std::string_view,4> columns(const std::string& line,const fs::path& file,std::size_t row) {
    std::array<std::string_view,4> result{};
    std::string_view remaining{line};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto comma = remaining.find(',');
        if ((i < 3 && comma == std::string_view::npos) || (i == 3 && comma != std::string_view::npos)) {
            fail(file,"line "+std::to_string(row),"expected exactly 4 CSV columns");
        }
        result[i] = trim(remaining.substr(0,comma));
        if (comma != std::string_view::npos) { remaining.remove_prefix(comma+1); }
    }
    return result;
}
std::uint64_t csv_integer(std::string_view value,const fs::path& file,const std::string& path,std::uint64_t max,bool positive) {
    if (value.empty()) { fail(file,path,"empty integer field"); }
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(),value.data()+value.size(),result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size() || result > max || (positive && result == 0)) {
        fail(file,path,"expected decimal integer in allowed range");
    }
    return result;
}
std::vector<DeliveryRecord> read_delivery(const fs::path& file,const std::set<NodeId>& nodes,SimTime horizon,SimDuration period) {
    std::ifstream stream{file};
    if (!stream) { fail(file,"line 1","cannot open delivery trace"); }
    std::string line;
    if (!std::getline(stream,line)) { fail(file,"line 1","CSV header missing"); }
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    if (line.starts_with("\xEF\xBB\xBF")) { line.erase(0,3); }
    const auto header = columns(line,file,1);
    const std::array<std::string_view,4> expected{"node_id","heartbeat_seq","delay_ticks","lost"};
    if (header != expected) { fail(file,"line 1","expected header node_id,heartbeat_seq,delay_ticks,lost"); }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto required = static_cast<std::uint64_t>(horizon.ticks()/period.ticks());
    std::map<std::pair<NodeId,HeartbeatSeq>,std::size_t> first_rows;
    std::map<NodeId,std::uint64_t> counts;
    for (const auto node : nodes) { counts.emplace(node,0); }
    std::vector<DeliveryRecord> result;
    std::size_t row = 1;
    while (std::getline(stream,line)) {
        ++row;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        const auto fields = columns(line,file,row);
        const auto location = "line "+std::to_string(row)+", ";
        const NodeId node{csv_integer(fields[0],file,location+"node_id",maximum,true)};
        if (!nodes.contains(node)) { fail(file,location+"node_id","unknown node"); }
        const HeartbeatSeq seq{csv_integer(fields[1],file,location+"heartbeat_seq",required,true)};
        const SimDuration delay{csv_integer(fields[2],file,location+"delay_ticks",
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),false)};
        if (fields[3] != "0" && fields[3] != "1") { fail(file,location+"lost","expected 0 or 1"); }
        const auto [seen,inserted] = first_rows.emplace(std::pair{node,seq},row);
        if (!inserted) { fail(file,location+"node_id/heartbeat_seq","duplicate key first seen on line "+std::to_string(seen->second)); }
        counts.at(node) = checked_increment(counts.at(node));
        result.push_back({node,seq,delay,fields[3] == "1"});
    }
    if (!stream.eof()) { fail(file,"line "+std::to_string(row+1),"read error"); }
    for (const auto& [node,count] : counts) {
        if (count != required) { fail(file,"heartbeat_seq for node_id="+std::to_string(node.value()),
            "incomplete trace: expected "+std::to_string(required)+" records, got "+std::to_string(count)); }
    }
    return result;
}
}  // namespace
namespace {
LoadedScenario load_scenario_impl(const fs::path& requested,const fs::path* trace_override) {
    if (requested.empty() || requested.native().find(fs::path::value_type{}) != fs::path::string_type::npos) {
        fail(requested,"$","invalid empty/NUL scenario path");
    }
    std::error_code error;
    const auto file = fs::absolute(requested,error).lexically_normal();
    if (error) { fail(requested,"$","cannot resolve scenario path: "+error.message()); }
    std::ifstream stream{file};
    if (!stream) { fail(file,"$","cannot open scenario JSON"); }
    Reader reader{file};
    if (!Json::sax_parse(stream,&reader)) { fail(file,"$","invalid JSON"); }
    if (stream.bad()) { fail(file,"$","JSON read error"); }
    const auto json = reader.take();
    object(json,file,"$",{"schema_version","tick_ns","horizon_ticks","heartbeat_period_ticks","tau0_ticks","kappa",
        "adaptive","nodes","tasks","crashes","blackouts","measurement_windows","delivery_trace"});
    if (integer(json.at("schema_version"),file,"$.schema_version") != 1) { fail(file,"$.schema_version","only version 1 is supported"); }
    if (integer(json.at("tick_ns"),file,"$.tick_ns") != static_cast<std::uint64_t>(tick_ns)) { fail(file,"$.tick_ns","must be 1000"); }
    const auto horizon = time(json.at("horizon_ticks"),file,"$.horizon_ticks");
    if (horizon == SimTime{}) { fail(file,"$.horizon_ticks","must be positive"); }
    const SimDuration period{time(json.at("heartbeat_period_ticks"),file,"$.heartbeat_period_ticks").ticks()};
    if (period == SimDuration{}) { fail(file,"$.heartbeat_period_ticks","must be positive"); }
    const SimDuration initial{time(json.at("tau0_ticks"),file,"$.tau0_ticks").ticks()};
    if (initial < period) { fail(file,"$.tau0_ticks","must be at least heartbeat_period_ticks"); }
    const auto kappa = real(json.at("kappa"),file,"$.kappa",true);
    if (kappa <= 1) { fail(file,"$.kappa","must exceed 1"); }
    static_cast<void>(checked(file,"$.kappa",[&] {
        return round_thresholds(static_cast<long double>(initial.ticks()),kappa*static_cast<long double>(initial.ticks()));
    }));
    const auto& adaptive_json = json.at("adaptive");
    object(adaptive_json,file,"$.adaptive",{"window","min_intervals","c"});
    const auto max_size = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    const auto window = integer(adaptive_json.at("window"),file,"$.adaptive.window",max_size);
    const auto minimum = integer(adaptive_json.at("min_intervals"),file,"$.adaptive.min_intervals",max_size);
    if (minimum < 2) { fail(file,"$.adaptive.min_intervals","must be at least 2"); }
    if (window < minimum) { fail(file,"$.adaptive.window","must be at least min_intervals"); }
    const core::AdaptiveTimeout adaptive{static_cast<std::size_t>(window),static_cast<std::size_t>(minimum),
        real(adaptive_json.at("c"),file,"$.adaptive.c",false)};
    PhysicalInput physical{period,{},{},{},{}};
    std::set<NodeId> nodes;
    const auto& node_array = array(json.at("nodes"),file,"$.nodes");
    if (node_array.empty()) { fail(file,"$.nodes","must contain at least one node"); }
    for (std::size_t i = 0; i < node_array.size(); ++i) {
        const auto path = "$.nodes["+std::to_string(i)+"]";
        const auto& node = node_array[i]; object(node,file,path,{"id","performance_work_per_second"});
        const NodeId id{integer(node.at("id"),file,path+".id",std::numeric_limits<std::uint64_t>::max(),true)};
        if (!nodes.insert(id).second) { fail(file,path+".id","duplicate node ID"); }
        physical.nodes.push_back({id,real(node.at("performance_work_per_second"),file,path+".performance_work_per_second",true)});
    }
    std::vector<TaskDescription> tasks;
    std::set<TaskId> task_ids;
    const auto& task_array = array(json.at("tasks"),file,"$.tasks");
    const auto [slow,fast] = std::minmax_element(physical.nodes.begin(),physical.nodes.end(),[](const auto& a,const auto& b) {
        return a.performance < b.performance;
    });
    for (std::size_t i = 0; i < task_array.size(); ++i) {
        const auto path = "$.tasks["+std::to_string(i)+"]";
        const auto& task = task_array[i]; object(task,file,path,{"id","arrival_ticks","work"});
        const TaskId id{integer(task.at("id"),file,path+".id",std::numeric_limits<std::uint64_t>::max(),true)};
        if (!task_ids.insert(id).second) { fail(file,path+".id","duplicate task ID"); }
        const auto arrival = time(task.at("arrival_ticks"),file,path+".arrival_ticks");
        if (arrival > horizon) { fail(file,path+".arrival_ticks","beyond horizon"); }
        const auto work = real(task.at("work"),file,path+".work",true);
        checked(file,path+".work",[&] { static_cast<void>(processing_duration(work,slow->performance));
            static_cast<void>(processing_duration(work,fast->performance)); });
        tasks.push_back({id,arrival,work});
    }
    const auto known_node = [&](const Json& value,const std::string& path) {
        const NodeId id{integer(value,file,path,std::numeric_limits<std::uint64_t>::max(),true)};
        if (!nodes.contains(id)) { fail(file,path,"unknown node"); }
        return id;
    };
    const auto& crash_array = array(json.at("crashes"),file,"$.crashes");
    std::set<NodeId> crashed;
    for (std::size_t i = 0; i < crash_array.size(); ++i) {
        const auto path = "$.crashes["+std::to_string(i)+"]";
        const auto& crash = crash_array[i]; object(crash,file,path,{"node_id","at_ticks"});
        const auto node = known_node(crash.at("node_id"),path+".node_id");
        if (!crashed.insert(node).second) { fail(file,path+".node_id","duplicate crash"); }
        const auto at = time(crash.at("at_ticks"),file,path+".at_ticks");
        if (at > horizon) { fail(file,path+".at_ticks","beyond horizon"); }
        physical.crashes.push_back({node,at});
    }
    const auto& blackout_array = array(json.at("blackouts"),file,"$.blackouts");
    for (std::size_t i = 0; i < blackout_array.size(); ++i) {
        const auto path = "$.blackouts["+std::to_string(i)+"]";
        const auto& blackout = blackout_array[i]; object(blackout,file,path,{"node_id","start_ticks","end_ticks"});
        const auto node = known_node(blackout.at("node_id"),path+".node_id");
        const auto start = time(blackout.at("start_ticks"),file,path+".start_ticks");
        const auto end = time(blackout.at("end_ticks"),file,path+".end_ticks");
        if (start >= end) { fail(file,path+".end_ticks","must exceed start_ticks"); }
        physical.blackouts.push_back({node,start,end});
    }
    std::vector<MeasurementWindow> windows;
    const auto& window_array = array(json.at("measurement_windows"),file,"$.measurement_windows");
    for (std::size_t i = 0; i < window_array.size(); ++i) {
        const auto path = "$.measurement_windows["+std::to_string(i)+"]";
        const auto& item = window_array[i]; object(item,file,path,{"start_ticks","end_ticks"});
        const auto start = time(item.at("start_ticks"),file,path+".start_ticks");
        const auto end = time(item.at("end_ticks"),file,path+".end_ticks");
        if (start >= end || end > horizon) { fail(file,path+".end_ticks","require start < end <= horizon"); }
        windows.push_back({start,end});
    }
    const auto& trace_json = json.at("delivery_trace");
    if (!trace_json.is_string()) { fail(file,"$.delivery_trace","expected nonempty path string"); }
    const auto trace_name = trace_json.get<std::string>();
    if (trace_name.empty() || trace_name.find('\0') != std::string::npos) { fail(file,"$.delivery_trace","invalid empty/NUL path"); }
    if (trace_override && (trace_override->empty() || trace_override->native().find(fs::path::value_type{}) != fs::path::string_type::npos)) {
        fail(*trace_override,"delivery_trace","invalid empty/NUL override path");
    }
    const auto delivery_file = trace_override ? fs::absolute(*trace_override).lexically_normal() : (file.parent_path()/fs::path{trace_name}).lexically_normal();
    physical.deliveries = read_delivery(delivery_file,nodes,horizon,period);
    auto trace = checked(file,"$",[&] { return std::make_shared<const PhysicalTrace>(horizon,std::move(physical)); });
    auto scenario = checked(file,"$",[&] { return std::make_shared<const Scenario>(std::move(trace),
        core::TimeoutParameters{period,initial,kappa},adaptive,std::move(tasks),std::move(windows)); });
    return {std::move(scenario),file,delivery_file};
}
}  // namespace
LoadedScenario load_scenario(const fs::path& requested) { return load_scenario_impl(requested,nullptr); }
LoadedScenario load_scenario(const fs::path& requested,const fs::path& trace_override) { return load_scenario_impl(requested,&trace_override); }
}  // namespace telemetry::io
