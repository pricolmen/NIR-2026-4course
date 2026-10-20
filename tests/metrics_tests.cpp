#include "telemetry/io/journal.hpp"
#include "telemetry/io/run_output.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {
using namespace telemetry;
using namespace telemetry::simulation;
namespace fs = std::filesystem;
fs::path data;
void require(bool value,std::string_view message) { if (!value) { throw std::runtime_error(std::string{message}); } }
template<class E = std::exception,class F> void rejects(F action) {
    try { action(); } catch (const E&) { return; } throw std::runtime_error("Expected rejection");
}
std::string bytes(const fs::path& file) { std::ifstream stream{file,std::ios::binary}; require(static_cast<bool>(stream),"Read fixture"); return {std::istreambuf_iterator<char>{stream},{}}; }
void put(const fs::path& file,const std::string& value) { std::ofstream stream{file,std::ios::binary}; stream << value; require(static_cast<bool>(stream),"Write fixture"); }
PhysicalInput input(int end,int count = 2,bool lost_first = true,int period = 1) {
    PhysicalInput value{SimDuration{period},{},{},{},{}};
    for (int id = 1; id <= count; ++id) {
        value.nodes.push_back({NodeId{id},1'000'000});
        for (int seq = 1; seq <= end/period; ++seq) { value.deliveries.push_back({NodeId{id},HeartbeatSeq{seq},SimDuration{0},lost_first && id == 1}); }
    }
    return value;
}
TaskDescription task(int id,int at,long double work) { return {TaskId{id},SimTime{at},work}; }
std::shared_ptr<const Scenario> scenario(int end,PhysicalInput value,std::vector<TaskDescription> tasks = {},int tau = 4,
    std::vector<MeasurementWindow> windows = {}) {
    const auto period = value.heartbeat_period;
    if (windows.empty()) { windows.push_back({SimTime{0},SimTime{end}}); }
    return std::make_shared<const Scenario>(std::make_shared<const PhysicalTrace>(SimTime{end},std::move(value)),
        core::TimeoutParameters{period,SimDuration{tau},2},core::AdaptiveTimeout{3,2,2},std::move(tasks),std::move(windows));
}
std::shared_ptr<const Scenario> example(std::string_view name) {
    return io::load_scenario(fs::path{TELEMETRY_SOURCE_DIR}/"examples"/name/"scenario.json").scenario;
}
struct Tee : SimulationAuditSink {
    metrics::Analyzer& analyzer; io::JournalWriter& writer;
    Tee(metrics::Analyzer& a,io::JournalWriter& w) : analyzer(a),writer(w) {}
    void on_event(const engine::EventKey& key,const SimulationEvent& event) override { analyzer.on_event(key,event); writer.on_event(key,event); }
};
std::pair<metrics::Analysis,std::string> run(std::shared_ptr<const Scenario> value,Variant variant = Variant::fixed_rr) {
    std::ostringstream text; io::JournalWriter writer{text}; metrics::Analyzer analyzer{value}; Tee sink{analyzer,writer};
    Simulation simulation{value,variant,&sink}; const auto completed = simulation.run(); writer.complete(completed);
    auto result = analyzer.finish(completed); std::istringstream stored{text.str()}; metrics::Analyzer replay{value};
    const auto end = io::read_journal(stored,replay); require(replay.finish(end) == result,"Independent full replay differs");
    return {std::move(result),text.str()};
}
void baseline() {
    const auto [report,text] = run(example("baseline")); static_cast<void>(text);
    const auto& s = report.summary;
    require(s.completed == 4 && s.unfinished == 0 && s.arrived == 4 && s.retries == 0,"Baseline balance");
    require(s.mean_latency_ticks == 4.5L && s.p95_latency == SimDuration{8},"Baseline latency");
    require(s.throughput.size() == 1 && s.throughput[0].completed == 4 && s.throughput[0].tasks_per_second == 200'000,"Baseline throughput");
    require(s.detection_samples == 0 && !s.mean_detection_ticks && !s.mean_recovery_ticks,"No failures is empty sample");
}
std::shared_ptr<const Scenario> recovery_scenario(int horizon,bool empty = false) {
    auto value = input(horizon); value.crashes.push_back({NodeId{1},SimTime{3}});
    return scenario(horizon,std::move(value),empty ? std::vector<TaskDescription>{} : std::vector<TaskDescription>{task(1,0,6),task(2,0,1),task(3,0,4)});
}
void recovery() {
    const auto [report,text] = run(recovery_scenario(20)); static_cast<void>(text);
    const auto& node = report.nodes.at(NodeId{1});
    require(node.failed_at == SimTime{8} && node.returned == std::vector<TaskId>{TaskId{1},TaskId{3}},"Recovery frozen set");
    require(report.tasks.at(TaskId{1}).completed_at == SimTime{14} && report.tasks.at(TaskId{3}).completed_at == SimTime{18},"Recovery manual completion");
    require(node.detection.value == SimDuration{5} && node.recovery.value == SimDuration{15},"Manual Detection 5 Recovery 15");
    require(report.summary.detection_samples == 1 && report.summary.recovery_samples == 1,"Detection/recovery sample counts");
    const auto empty = run(recovery_scenario(20,true)).first.nodes.at(NodeId{1});
    require(empty.returned.empty() && empty.recovery.value == SimDuration{5},"Empty recovery equals detection");
    const auto incomplete = run(recovery_scenario(17)).first;
    require(!incomplete.nodes.at(NodeId{1}).recovery.value && incomplete.nodes.at(NodeId{1}).recovery.reason == "recovery_incomplete" &&
        incomplete.summary.incomplete_recoveries == 1 && incomplete.summary.recovery_samples == 0,"Incomplete recovery must be missing");
    auto extra_input = input(22); extra_input.crashes.push_back({NodeId{1},SimTime{3}});
    const auto extra = run(scenario(22,std::move(extra_input),{task(1,0,6),task(2,0,1),task(3,3,1)})).first;
    require(extra.nodes.at(NodeId{1}).returned == std::vector<TaskId>{TaskId{1},TaskId{3}},"Assignment after physical crash enters recovery set");
    auto chain = input(40,3); chain.crashes = {{NodeId{1},SimTime{3}},{NodeId{2},SimTime{10}}};
    const auto chained = run(scenario(40,std::move(chain),{task(1,0,20)})).first;
    require(chained.tasks.at(TaskId{1}).attempts == 3 && chained.tasks.at(TaskId{1}).completed_at == SimTime{37} &&
        chained.nodes.at(NodeId{1}).returned == std::vector<TaskId>{TaskId{1}} &&
        chained.nodes.at(NodeId{1}).recovery.value == SimDuration{34} && chained.nodes.at(NodeId{2}).recovery.value == SimDuration{27},
        "Recovery membership must survive subsequent reassignment");
}
void classification() {
    auto late = input(20); late.crashes.push_back({NodeId{1},SimTime{12}});
    const auto report = run(scenario(20,std::move(late))).first;
    const auto& s = report.summary; const auto& node = report.nodes.at(NodeId{1});
    require(s.false_positives == 1 && s.crashes == 1 && s.crashes_after_false_exclusion == 1 && s.false_negatives == 0 && s.detection_samples == 0,"FP then crash classification");
    require(node.classification == "crash_after_false_exclusion" && node.detection.reason == "crash_after_false_exclusion" && !node.recovery.value,"Late crash must not reclassify FP");
    auto near = input(6); near.crashes.push_back({NodeId{1},SimTime{6}});
    const auto missed = run(scenario(6,std::move(near))).first;
    require(missed.summary.false_negatives == 1 && missed.nodes.at(NodeId{1}).detection.reason == "not_detected","FN at horizon");
    const auto fp = run(scenario(12,input(12))).first;
    require(fp.summary.false_positives == 1 && fp.summary.crashes == 0 && fp.nodes.at(NodeId{1}).classification == "false_positive","Healthy physical node false FAILED");
    auto exact = input(8); exact.crashes.push_back({NodeId{1},SimTime{8}});
    const auto simultaneous = run(scenario(8,std::move(exact))).first;
    require(simultaneous.summary.false_positives == 0 && simultaneous.nodes.at(NodeId{1}).detection.value == SimDuration{0},"Crash phase1 at FAILED phase5 is true positive");
}
void boundaries() {
    require(!metrics::p95_rank(0) && metrics::p95_rank(1) == 1 && metrics::p95_rank(19) == 19 && metrics::p95_rank(20) == 19 &&
        metrics::p95_rank(21) == 20 && metrics::p95_rank(std::numeric_limits<std::size_t>::max()) ==
        std::numeric_limits<std::size_t>::max()-std::numeric_limits<std::size_t>::max()/20,"P95 rank overflow/boundaries");
    const auto empty = run(scenario(4,input(4,1,false))).first;
    require(!empty.summary.mean_latency_ticks && !empty.summary.p95_latency && empty.summary.throughput[0].tasks_per_second == 0,"Empty samples and zero throughput");
    const auto report = run(scenario(6,input(6,1,false),{task(1,0,2),task(2,0,2),task(3,6,1)},4,
        {{SimTime{2},SimTime{4}},{SimTime{4},SimTime{6}}})).first;
    require(report.summary.throughput[0].completed == 1 && report.summary.throughput[0].tasks_per_second == 500'000 &&
        report.summary.throughput[1].completed == 0,"Throughput window excludes start and includes end");
    require(report.summary.unfinished == 1 && report.summary.assigned == 1 && report.tasks.at(TaskId{3}).attempts == 1,"Horizon unfinished is not lost");
    constexpr std::int64_t large = 9007199254740993LL;
    const auto maximal = std::numeric_limits<std::uint64_t>::max();
    PhysicalInput huge{SimDuration{large},{{NodeId{maximal},1'000'000}},{},{},{{NodeId{maximal},HeartbeatSeq{1},SimDuration{0},false}}};
    const auto large_trace = std::make_shared<const PhysicalTrace>(SimTime{large+3},std::move(huge));
    const auto precise = run(std::make_shared<const Scenario>(large_trace,core::TimeoutParameters{SimDuration{large},SimDuration{large},2},
        core::AdaptiveTimeout{3,2,2},std::vector<TaskDescription>{{TaskId{maximal},SimTime{large},1}},std::vector<MeasurementWindow>{{SimTime{large},SimTime{large+3}}})).first;
    require(precise.tasks.at(TaskId{maximal}).completed_at == SimTime{large+1},"Ticks above double precision and maximal IDs");
    const auto single = run(scenario(4,input(4,1,false),{task(1,0,1)})).first;
    require(single.summary.p95_latency == SimDuration{1} && single.summary.mean_latency_ticks == 1,"Single sample");
}
void retries() {
    // Three nodes: node 1 falsely fails at 8; node 2's last HB at 8 gives
    // FAILED at 16. A long task is assigned three times, still unfinished.
    auto value = input(20,3);
    for (auto& delivery : value.deliveries) { if (delivery.node == NodeId{2} && delivery.sequence.value() > 8) { delivery.lost = true; } }
    const auto report = run(scenario(20,std::move(value),{task(1,0,100)})).first;
    require(report.tasks.at(TaskId{1}).attempts == 3 && report.summary.retries == 2 && report.summary.unfinished == 1,"Three attempts mean two retries");
    auto old = input(24);
    const auto late = run(scenario(24,std::move(old),{task(1,0,10),task(2,0,1),task(3,0,3)})).first;
    require(late.summary.rejected_revoked_results == 2 && late.summary.completed == 3 && late.summary.retries == 2,"Revoked physical results must not corrupt task counts");
}
struct CopySink : SimulationAuditSink {
    io::JournalWriter& writer; explicit CopySink(io::JournalWriter& w) : writer(w) {}
    void on_event(const engine::EventKey& key,const SimulationEvent& event) override { writer.on_event(key,event); }
};
void journal() {
    for (const auto name : {"baseline","crash","blackout"}) {
        const auto shared = example(name);
        for (const auto variant : {Variant::fixed_rr,Variant::fixed_ll,Variant::adaptive_rr,Variant::adaptive_ll}) {
            const auto [analysis,text] = run(shared,variant); static_cast<void>(analysis);
            std::istringstream input_stream{text}; std::ostringstream copied; io::JournalWriter writer{copied}; CopySink sink{writer};
            const auto end = io::read_journal(input_stream,sink); writer.complete(end);
            require(copied.str() == text,"Every field must round-trip canonically");
            require(text.find("raw_suspect_ticks_decimal") != std::string::npos && text.find("action_sequence") != std::string::npos &&
                text.find("stale_generation") != std::string::npos,"Detector diagnostics and key must be present");
        }
    }
}
void diagnostic_journal() {
    auto value = input(24,2,true,2); value.blackouts.push_back({NodeId{1},SimTime{2},SimTime{4}});
    value.crashes.push_back({NodeId{1},SimTime{20}});
    for (auto& delivery : value.deliveries) {
        if (delivery.node == NodeId{1} && delivery.sequence == HeartbeatSeq{1}) { delivery.lost = false; }
        if (delivery.node == NodeId{1} && delivery.sequence == HeartbeatSeq{5}) { delivery.lost = false; delivery.delay = SimDuration{4}; }
        if (delivery.node == NodeId{2} && delivery.sequence == HeartbeatSeq{1}) { delivery.delay = SimDuration{5}; }
        if (delivery.node == NodeId{2} && delivery.sequence == HeartbeatSeq{12}) { delivery.delay = SimDuration{1}; }
    }
    const auto text = run(scenario(24,std::move(value),{task(1,0,22),task(2,0,1)})).second;
    for (const auto* token : {"non_increasing_sequence","already_failed","blackout","trace_lost","beyond_horizon","completion_skipped"}) {
        require(text.find(token) != std::string::npos,"Missing auxiliary journal diagnostic");
    }
}
void materialize(const fs::path& path,const Scenario& value) {
    fs::create_directories(path); std::ofstream stream{path/"scenario.json"}; stream << std::setprecision(std::numeric_limits<long double>::max_digits10);
    stream << "{\"schema_version\":1,\"tick_ns\":1000,\"horizon_ticks\":" << value.physical()->horizon().ticks()
        << ",\"heartbeat_period_ticks\":" << value.timeout().heartbeat_period.ticks() << ",\"tau0_ticks\":" << value.timeout().initial_timeout.ticks()
        << ",\"kappa\":2,\"adaptive\":{\"window\":3,\"min_intervals\":2,\"c\":2},\"nodes\":[";
    bool first = true;
    for (auto node : value.physical()->nodes()) { if (!first) { stream << ','; } first = false; stream << "{\"id\":" << node.id.value() << ",\"performance_work_per_second\":" << node.performance << '}'; }
    stream << "],\"tasks\":["; first = true;
    for (auto task_value : value.tasks()) { if (!first) { stream << ','; } first = false; stream << "{\"id\":" << task_value.id.value() << ",\"arrival_ticks\":" << task_value.arrival.ticks() << ",\"work\":" << task_value.work << '}'; }
    stream << "],\"crashes\":["; first = true;
    for (auto crash : value.physical()->crashes()) { if (!first) { stream << ','; } first = false; stream << "{\"node_id\":" << crash.node.value() << ",\"at_ticks\":" << crash.at.ticks() << '}'; }
    stream << "],\"blackouts\":[],\"measurement_windows\":[{\"start_ticks\":0,\"end_ticks\":" << value.physical()->horizon().ticks() << "}],\"delivery_trace\":\"delivery.csv\"}";
    require(static_cast<bool>(stream),"Materialize JSON"); stream.close();
    std::ofstream trace{path/"delivery.csv"}; trace << "node_id,heartbeat_seq,delay_ticks,lost\n";
    for (auto row : value.physical()->deliveries()) { trace << row.node.value() << ',' << row.sequence.value() << ',' << row.delay.ticks() << ',' << row.lost << '\n'; }
}
void files() {
    const auto out = data/"baseline";
    const auto analysis = io::write_run(fs::path{TELEMETRY_SOURCE_DIR}/"examples/baseline/scenario.json",Variant::fixed_rr,out);
    require(io::analyze_run(out) == analysis,"Saved run analysis");
    for (const auto name : {"manifest.json","events.jsonl","tasks.csv","attempts.csv","nodes.csv","metrics.json"}) { require(fs::file_size(out/name) > 0,"Missing output"); }
    const auto before = bytes(out/"metrics.json"); io::recalculate_reports(out); require(bytes(out/"metrics.json") == before,"Recalculation changes metrics");
    put(out/"metrics.json","corrupt derived result"); io::recalculate_reports(out); require(bytes(out/"metrics.json") == before,"Recalculation must use journal, not cached metrics");
    rejects([&] { static_cast<void>(io::write_run(fs::path{TELEMETRY_SOURCE_DIR}/"examples/baseline/scenario.json",Variant::fixed_rr,out)); });
    materialize(data/"source",*recovery_scenario(20));
    const auto full = io::write_run(data/"source/scenario.json",Variant::fixed_rr,data/"recovery");
    fs::rename(data/"source",data/"source-moved"); fs::rename(data/"recovery",data/"run-moved");
    require(io::analyze_run(data/"run-moved") == full,"Snapshots must survive moved inputs/results");
    require(bytes(data/"run-moved/input/scenario.json") == bytes(data/"source-moved/scenario.json"),"Snapshot JSON bytes changed");
    const auto precise = scenario(10,input(10,1,false),{task(1,0,std::nextafter(4.0L,5.0L))});
    materialize(data/"precision-source",*precise);
    const auto precision = io::write_run(data/"precision-source/scenario.json",Variant::fixed_rr,data/"precision-run");
    require(precision.tasks.at(TaskId{1}).description.work == precise->tasks()[0].work && io::analyze_run(data/"precision-run") == precision,
        "Snapshots and decimal journal must preserve native real precision");
}
std::string replace(std::string text,std::string_view from,std::string_view to) {
    const auto pos = text.find(from); require(pos != std::string::npos,"Mutation token absent"); text.replace(pos,from.size(),to); return text;
}
void errors() {
    const auto shared = example("baseline"); const auto text = run(shared).second;
    const auto reject_journal = [&](const std::string& bad) {
        rejects([&] { std::istringstream stream{bad}; metrics::Analyzer analyzer{shared}; const auto end = io::read_journal(stream,analyzer); static_cast<void>(analyzer.finish(end)); });
    };
    reject_journal(text.substr(0,text.find_last_of('\n',text.size()-2)+1));
    reject_journal(text+"{}\n"); reject_journal(replace(text,"\"record_no\":2,","\"record_no\":3,"));
    reject_journal(replace(text,"\"time_ticks\":0","\"time_ticks\":0.0"));
    reject_journal(replace(text,"\"schema_version\":1","\"schema_version\":1,\"schema_version\":1"));
    reject_journal(replace(text,"task_arrived","unknown_event")); reject_journal(replace(text,"\"kind\":\"task_arrived\"","\"extra\":0,\"kind\":\"task_arrived\""));
    reject_journal(replace(text,"\"phase\":7","\"phase\":6"));
    reject_journal(replace(text,"\"decision\":\"accepted\"","\"decision\":\"bogus\""));
    reject_journal(replace(text,"\"phase\":0","\"phase\":0.0"));
    // Analyzer lifecycle remains closed after successful completion.
    metrics::Analyzer analyzer{shared}; Simulation simulation{shared,Variant::fixed_rr,&analyzer}; const auto end = simulation.run();
    static_cast<void>(analyzer.finish(end)); rejects<std::logic_error>([&] { static_cast<void>(analyzer.finish(end)); });
    std::ostringstream broken; io::JournalWriter writer{broken}; broken.setstate(std::ios::badbit);
    rejects([&] { writer.complete({SimTime{20},0}); }); rejects([&] { writer.complete({SimTime{20},0}); });
    const auto out = data/"corrupt"; static_cast<void>(io::write_run(fs::path{TELEMETRY_SOURCE_DIR}/"examples/baseline/scenario.json",Variant::fixed_rr,out));
    const auto manifest = bytes(out/"manifest.json"); put(out/"manifest.json",replace(manifest,"\"complete\"","\"incomplete\"")); rejects([&] { static_cast<void>(io::analyze_run(out)); });
    put(out/"manifest.json",manifest); put(out/"events.jsonl",text+"\n"); rejects([&] { static_cast<void>(io::analyze_run(out)); });
    put(out/"events.jsonl",text); put(out/"input/delivery.csv",bytes(out/"input/delivery.csv")+"\n"); rejects([&] { static_cast<void>(io::analyze_run(out)); });
    rejects<std::invalid_argument>([] { metrics::Analyzer null{nullptr}; });
}
void reproducibility() {
    const auto source = fs::path{TELEMETRY_SOURCE_DIR}/"examples/crash/scenario.json";
    const std::array variants{Variant::fixed_rr,Variant::fixed_ll,Variant::adaptive_rr,Variant::adaptive_ll};
    std::map<Variant,metrics::Analysis> results;
    for (std::size_t i = 0; i < variants.size(); ++i) { results.emplace(variants[i],io::write_run(source,variants[i],data/("first"+std::to_string(i)))); }
    for (std::size_t i = variants.size(); i > 0; --i) {
        const auto index = i-1; const auto name = std::to_string(index);
        const auto repeat = io::write_run(source,variants[index],data/("second"+name)); require(repeat == results.at(variants[index]),"Variant order changed metrics");
        for (const auto file : {"manifest.json","events.jsonl","metrics.json","tasks.csv","attempts.csv","nodes.csv"}) {
            require(bytes(data/("first"+name)/file) == bytes(data/("second"+name)/file),"Canonical saved output differs");
        }
    }
}
}
int main(int argc,char** argv) {
    try {
        if (argc != 2) { throw std::invalid_argument("Expected group"); }
        data = fs::path{TELEMETRY_TEST_BINARY_DIR}/"stage11-tests"/argv[1];
        fs::remove_all(data); fs::create_directories(data);
        const std::map<std::string_view,void(*)()> groups{{"baseline",baseline},{"recovery",recovery},{"classification",classification},{"boundaries",boundaries},
            {"retries",retries},{"journal",+[] { journal(); diagnostic_journal(); }},{"files",files},{"errors",errors},{"reproducibility",reproducibility}};
        groups.at(argv[1])();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
