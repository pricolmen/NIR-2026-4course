#include "telemetry/io/experiments.hpp"
#include "telemetry/io/build_info.hpp"
#include "scenario_json.hpp"
#include "file_support.hpp"
#include <random>

namespace telemetry::io {
namespace fs = std::filesystem;
GenerationPlan load_generation_plan(const fs::path& requested) {
    using namespace input_detail;
    const auto file = fs::absolute(requested).lexically_normal(); const auto hash = detail::checksum(file); const auto json = load(file);
    object(json,file,"$",{"schema_version","seed","scenario","tasks","delivery"});
    if (integer(json.at("schema_version"),file,"$.schema_version") != 1) { fail(file,"$.schema_version","only version 1 is supported"); }
    const auto& s = json.at("scenario"); const auto& t = json.at("tasks"); const auto& d = json.at("delivery");
    object(s,file,"$.scenario",{"tick_ns","horizon_ticks","heartbeat_period_ticks","tau0_ticks","kappa","adaptive","nodes","crashes","blackouts","measurement_windows"});
    object(t,file,"$.tasks",{"count","first_arrival_ticks","period_ticks","work_min","work_max"});
    object(d,file,"$.delivery",{"min_delay_ticks","max_delay_ticks","loss_probability"});
    if (integer(s.at("tick_ns"),file,"$.scenario.tick_ns") != 1000) { fail(file,"$.scenario.tick_ns","must be 1000"); }
    const auto horizon = time(s.at("horizon_ticks"),file,"$.scenario.horizon_ticks");
    const SimDuration period{time(s.at("heartbeat_period_ticks"),file,"$.scenario.heartbeat_period_ticks").ticks()};
    const SimDuration tau{time(s.at("tau0_ticks"),file,"$.scenario.tau0_ticks").ticks()};
    const auto& a = s.at("adaptive"); object(a,file,"$.scenario.adaptive",{"window","min_intervals","c"});
    const auto maximum_size = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    GenerationPlan plan{file,hash,integer(json.at("seed"),file,"$.seed"),horizon,{period,{},{},{},{}},
        {period,tau,real(s.at("kappa"),file,"$.scenario.kappa",true)},
        {static_cast<std::size_t>(integer(a.at("window"),file,"$.scenario.adaptive.window",maximum_size)),
         static_cast<std::size_t>(integer(a.at("min_intervals"),file,"$.scenario.adaptive.min_intervals",maximum_size)),real(a.at("c"),file,"$.scenario.adaptive.c",false)},
        {},integer(t.at("count"),file,"$.tasks.count"),time(t.at("first_arrival_ticks"),file,"$.tasks.first_arrival_ticks"),
        SimDuration{time(t.at("period_ticks"),file,"$.tasks.period_ticks").ticks()},
        SimDuration{time(d.at("min_delay_ticks"),file,"$.delivery.min_delay_ticks").ticks()},SimDuration{time(d.at("max_delay_ticks"),file,"$.delivery.max_delay_ticks").ticks()},
        real(t.at("work_min"),file,"$.tasks.work_min",true),real(t.at("work_max"),file,"$.tasks.work_max",true),real(d.at("loss_probability"),file,"$.delivery.loss_probability",false)};
    if (horizon == SimTime{} || period == SimDuration{} || plan.task_period == SimDuration{} || plan.first_arrival > horizon ||
        plan.min_work > plan.max_work || plan.min_delay > plan.max_delay || plan.loss_probability > 1) { fail(file,"$","invalid horizon/period/range/probability"); }
    if (plan.task_count > 0 && plan.task_count-1 > static_cast<std::uint64_t>((horizon-plan.first_arrival).ticks()/plan.task_period.ticks())) {
        fail(file,"$.tasks.count","periodic arrivals would exceed horizon");
    }
    std::set<NodeId> ids,crashed;
    const auto& nodes = array(s.at("nodes"),file,"$.scenario.nodes");
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto path = "$.scenario.nodes["+std::to_string(i)+"]"; const auto& node = nodes[i]; object(node,file,path,{"id","performance_work_per_second"});
        const NodeId id{integer(node.at("id"),file,path+".id",std::numeric_limits<std::uint64_t>::max(),true)};
        if (!ids.insert(id).second) { fail(file,path+".id","duplicate node"); }
        const auto p = real(node.at("performance_work_per_second"),file,path+".performance_work_per_second",true);
        checked(file,path,[&] { static_cast<void>(core::Detector{id,plan.timeout,plan.adaptive});
            static_cast<void>(processing_duration(plan.min_work,p)); static_cast<void>(processing_duration(plan.max_work,p)); });
        plan.physical.nodes.push_back({id,p});
    }
    if (ids.empty()) { fail(file,"$.scenario.nodes","must not be empty"); }
    std::sort(plan.physical.nodes.begin(),plan.physical.nodes.end(),[](auto x,auto y) { return x.id < y.id; });
    const auto known = [&](const Json& item,const std::string& path) {
        const NodeId id{integer(item.at("node_id"),file,path+".node_id",std::numeric_limits<std::uint64_t>::max(),true)};
        if (!ids.contains(id)) { fail(file,path+".node_id","unknown node"); } return id;
    };
    for (const auto& item : array(s.at("crashes"),file,"$.scenario.crashes")) {
        object(item,file,"$.scenario.crashes[]",{"node_id","at_ticks"}); const auto id = known(item,"$.scenario.crashes[]");
        const auto at = time(item.at("at_ticks"),file,"$.scenario.crashes[].at_ticks");
        if (at > horizon || !crashed.insert(id).second) { fail(file,"$.scenario.crashes[]","duplicate crash or beyond horizon"); }
        plan.physical.crashes.push_back({id,at});
    }
    for (const auto& item : array(s.at("blackouts"),file,"$.scenario.blackouts")) {
        object(item,file,"$.scenario.blackouts[]",{"node_id","start_ticks","end_ticks"}); const auto id = known(item,"$.scenario.blackouts[]");
        const auto start = time(item.at("start_ticks"),file,"$.scenario.blackouts[].start_ticks"),end = time(item.at("end_ticks"),file,"$.scenario.blackouts[].end_ticks");
        if (start >= end) { fail(file,"$.scenario.blackouts[]","require start < end"); } plan.physical.blackouts.push_back({id,start,end});
    }
    for (const auto& item : array(s.at("measurement_windows"),file,"$.scenario.measurement_windows")) {
        object(item,file,"$.scenario.measurement_windows[]",{"start_ticks","end_ticks"});
        const auto start = time(item.at("start_ticks"),file,"$.scenario.measurement_windows[].start_ticks"),end = time(item.at("end_ticks"),file,"$.scenario.measurement_windows[].end_ticks");
        if (start >= end || end > horizon) { fail(file,"$.scenario.measurement_windows[]","require start < end <= horizon"); } plan.windows.push_back({start,end});
    }
    if (detail::checksum(file) != hash) { fail(file,"$","config changed while reading"); } return plan;
}
void write_generated_scenario(const GenerationPlan& p,const fs::path& dir) {
    using namespace detail;
    require(!fs::exists(dir) || (fs::is_directory(dir) && fs::is_empty(dir))); fs::create_directories(dir);
    require(checksum(p.source) == p.source_checksum); fs::copy_file(p.source,dir/"generator_config.json"); require(checksum(dir/"generator_config.json") == p.source_checksum);
    Json metadata{{"schema_version",1},{"status","incomplete"},{"generator_version",1},{"seed",p.seed},{"rng","mt19937_64"},
        {"distributions","uniform_int<int64>;uniform_real<long double>;u<loss_probability"},{"config_checksum_fnv1a64",p.source_checksum},{"build",parse(build_info_json())}};
    save(dir/"generation_manifest.json",metadata);
    std::mt19937_64 rng{p.seed};
    std::uniform_real_distribution<long double> work{p.min_work,p.max_work},probability{0,1};
    std::uniform_int_distribution<std::int64_t> delay{p.min_delay.ticks(),p.max_delay.ticks()};
    auto stream = output(dir/"scenario.json");
    stream << "{\"schema_version\":1,\"tick_ns\":1000,\"horizon_ticks\":" << p.horizon.ticks() << ",\"heartbeat_period_ticks\":" << p.physical.heartbeat_period.ticks()
        << ",\"tau0_ticks\":" << p.timeout.initial_timeout.ticks() << ",\"kappa\":" << p.timeout.kappa
        << ",\"adaptive\":{\"window\":" << p.adaptive.window_size << ",\"min_intervals\":" << p.adaptive.min_intervals << ",\"c\":" << p.adaptive.coefficient << "},\"nodes\":[";
    bool first = true;
    for (auto node : p.physical.nodes) { if (!first) { stream << ','; } first = false; stream << "{\"id\":" << node.id.value() << ",\"performance_work_per_second\":" << node.performance << '}'; }
    stream << "],\"tasks\":["; SimTime at = p.first_arrival;
    for (std::uint64_t i = 0; i < p.task_count; ++i) {
        if (i) { stream << ','; } const auto amount = p.min_work == p.max_work ? p.min_work : std::clamp(work(rng),p.min_work,p.max_work);
        require(std::isfinite(amount) && amount > 0);
        stream << "{\"id\":" << checked_increment(i) << ",\"arrival_ticks\":" << at.ticks() << ",\"work\":" << amount << '}';
        if (i != p.task_count-1) { at = at+p.task_period; }
    }
    stream << "],\"crashes\":["; first = true;
    for (auto item : p.physical.crashes) { if (!first) { stream << ','; } first = false; stream << "{\"node_id\":" << item.node.value() << ",\"at_ticks\":" << item.at.ticks() << '}'; }
    stream << "],\"blackouts\":["; first = true;
    for (auto item : p.physical.blackouts) { if (!first) { stream << ','; } first = false; stream << "{\"node_id\":" << item.node.value() << ",\"start_ticks\":" << item.start.ticks() << ",\"end_ticks\":" << item.end.ticks() << '}'; }
    stream << "],\"measurement_windows\":["; first = true;
    for (auto item : p.windows) { if (!first) { stream << ','; } first = false; stream << "{\"start_ticks\":" << item.start.ticks() << ",\"end_ticks\":" << item.end.ticks() << '}'; }
    stream << "],\"delivery_trace\":\"delivery.csv\"}\n"; close(stream);
    auto trace = output(dir/"delivery.csv"); trace << "node_id,heartbeat_seq,delay_ticks,lost\n";
    const auto messages = static_cast<std::uint64_t>(p.horizon.ticks()/p.physical.heartbeat_period.ticks());
    for (auto node : p.physical.nodes) {
        for (std::uint64_t i = 0; i < messages; ++i) { const auto ticks = delay(rng); const auto draw = probability(rng); const bool lost = p.loss_probability == 1 || (p.loss_probability > 0 && draw < p.loss_probability);
            trace << node.id.value() << ',' << checked_increment(i) << ',' << ticks << ',' << (lost ? 1 : 0) << '\n'; }
    }
    close(trace); static_cast<void>(load_scenario(dir/"scenario.json"));
    metadata["scenario_checksum_fnv1a64"] = checksum(dir/"scenario.json"); metadata["delivery_checksum_fnv1a64"] = checksum(dir/"delivery.csv");
    metadata["status"] = "complete"; save(dir/"generation_manifest.json",metadata);
}
}  // namespace telemetry::io
