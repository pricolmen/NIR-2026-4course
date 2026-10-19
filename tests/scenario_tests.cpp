#include "telemetry/io/scenario_loader.hpp"
#include "telemetry/simulation/physical_environment.hpp"
#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace {
using namespace telemetry;
using namespace telemetry::io;
namespace fs = std::filesystem;
using Doc = nlohmann::json;
static_assert(!std::is_copy_assignable_v<simulation::Scenario>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<LoadedScenario>().scenario)>>);
fs::path directory;
void require(bool condition,std::string_view message) {
    if (!condition) { throw std::runtime_error(std::string{message}); }
}
void write(const fs::path& path,const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file{path,std::ios::binary}; file << text;
    require(static_cast<bool>(file),"Fixture write failed");
}
std::string read(const fs::path& path) {
    std::ifstream file{path,std::ios::binary}; return {std::istreambuf_iterator<char>{file},std::istreambuf_iterator<char>{}};
}
Doc document() {
    return {{"schema_version",1},{"tick_ns",1000},{"horizon_ticks",20},{"heartbeat_period_ticks",10},
        {"tau0_ticks",20},{"kappa",2},{"adaptive",{{"window",3},{"min_intervals",2},{"c",0}}},
        {"nodes",Doc::array({{{"id",2},{"performance_work_per_second",1000000}},{{"id",1},{"performance_work_per_second",1000000}}})},
        {"tasks",Doc::array({{{"id",2},{"arrival_ticks",5},{"work",2}},{{"id",3},{"arrival_ticks",0},{"work",4}},{{"id",1},{"arrival_ticks",0},{"work",1}}})},
        {"crashes",Doc::array()},{"blackouts",Doc::array()},{"measurement_windows",Doc::array({{{"start_ticks",0},{"end_ticks",20}}})},
        {"delivery_trace","delivery.csv"}};
}
const std::string csv = "node_id,heartbeat_seq,delay_ticks,lost\n2,2,0,0\n1,2,0,0\n2,1,0,0\n1,1,0,0\n";
fs::path fixture(const Doc& doc,const std::string& delivery = csv) {
    const auto file = directory/"scenario.json";
    write(file,doc.dump()); write(directory/"delivery.csv",delivery); return file;
}
void bad(const fs::path& file,std::string_view location,std::string_view source = "scenario.json") {
    try { static_cast<void>(load_scenario(file)); }
    catch (const InputError& error) {
        const std::string message{error.what()};
        require(message.find(location) != std::string::npos && message.find(source) != std::string::npos,
                "Error missing expected file/location: "+message);
        return;
    }
    throw std::runtime_error("Invalid fixture accepted: "+std::string{location});
}
void bad_doc(const Doc& value,std::string_view path) { bad(fixture(value),path); }
void valid() {
    auto value = document();
    value["delivery_trace"] = "../канал данных/delivery.csv";
    value["crashes"] = Doc::array({{{"node_id",1},{"at_ticks",0}}});
    value["blackouts"] = Doc::array({{{"node_id",2},{"start_ticks",100},{"end_ticks",200}},
        {{"node_id",1},{"start_ticks",10},{"end_ticks",15}},{{"node_id",1},{"start_ticks",12},{"end_ticks",18}}});
    const auto file = directory/"сценарий с пробелами"/"scenario.json";
    write(file,value.dump()); write(directory/"канал данных"/"delivery.csv",csv);
    const auto loaded = load_scenario(fs::relative(file));
    require(loaded.scenario_file == file && loaded.delivery_file == directory/"канал данных"/"delivery.csv", "Relative path resolved against cwd");
    const auto& scenario = *loaded.scenario;
    require(scenario.tasks().size() == 3 && scenario.tasks()[0].id == TaskId{1} && scenario.tasks()[1].id == TaskId{3} &&
            scenario.tasks()[2].arrival == SimTime{5}, "Tasks not canonical by arrival/ID");
    require(scenario.physical()->nodes()[0].id == NodeId{1} && scenario.physical()->blackouts().size() == 2 &&
            scenario.physical()->crashes()[0].at == SimTime{0}, "Physical input normalization/zero crash");
    require(scenario.timeout().kappa == 2 && scenario.adaptive().min_intervals == 2 && scenario.measurement_windows().size() == 1,
            "Scenario parameters lost");
    value = document(); value["tasks"] = Doc::array(); value["horizon_ticks"] = 5; value["measurement_windows"][0]["end_ticks"] = 5;
    require(load_scenario(fixture(value,"node_id,heartbeat_seq,delay_ticks,lost\n")).scenario->tasks().empty(), "Empty tasks/horizon<H rejected");
    value = document(); value["nodes"] = Doc::array(); value["horizon_ticks"] = UINT64_C(9007199254740993);
    value["tasks"][0]["arrival_ticks"] = UINT64_C(9007199254740993);
    require(load_scenario(fixture(value,"node_id,heartbeat_seq,delay_ticks,lost\n")).scenario->tasks().back().arrival.ticks() == INT64_C(9007199254740993),
            "Integer time went through floating point");
    value = document(); const auto maximum = std::numeric_limits<std::uint64_t>::max();
    value["nodes"] = Doc::array({{{"id",maximum},{"performance_work_per_second",1000000}}});
    value["tasks"][0]["id"] = maximum;
    const auto loaded_max = load_scenario(fixture(value,"node_id,heartbeat_seq,delay_ticks,lost\n"+std::to_string(maximum)+",1,0,0\n"+std::to_string(maximum)+",2,0,0\n"));
    require(loaded_max.scenario->physical()->nodes()[0].id.value() == maximum && loaded_max.scenario->tasks().back().id.value() == maximum,
            "Full unsigned ID range lost");
}
void json_checks() {
    auto value = document(); value.erase("nodes"); bad_doc(value,"$.nodes");
    value = document(); value["horizn_ticks"] = 20; bad_doc(value,"$.horizn_ticks");
    value = document(); value["adaptive"]["extra"] = 1; bad_doc(value,"$.adaptive.extra");
    value = document(); value["nodes"] = Doc::object(); bad_doc(value,"$.nodes");
    value = document(); value["nodes"][0] = nullptr; bad_doc(value,"$.nodes[0]");
    value = document(); value["tasks"][0]["arrival_ticks"] = 1.0; bad_doc(value,"$.tasks[0].arrival_ticks");
    value = document(); value["tasks"][0]["id"] = true; bad_doc(value,"$.tasks[0].id");
    value = document(); value["nodes"][0]["performance_work_per_second"] = "inf"; bad_doc(value,"$.nodes[0].performance_work_per_second");
    auto file = fixture(document());
    write(file,"{\"schema_version\":1,\"schema_version\":1}"); bad(file,"$.schema_version");
    write(file,"{\"x\":{\"id\":1,\"id\":2}}"); bad(file,"$.x.id");
    write(file,"{ broken JSON"); bad(file,"JSON byte");
    write(file,document().dump()+" trailing"); bad(file,"JSON byte");
    auto text = document().dump();
    auto pos = text.find("\"c\":0"); require(pos != std::string::npos,"Missing token fixture");
    text.replace(pos,5,"\"c\":1e-99999"); write(file,text); bad(file,"$.adaptive.c");
    text = document().dump(); pos = text.find("\"c\":0"); text.replace(pos,5,"\"c\":1e99999"); write(file,text); bad(file,"JSON byte");
    text = document().dump(); pos = text.find("\"arrival_ticks\":5");
    require(pos != std::string::npos,"Missing exponent fixture");
    text.replace(pos,17,"\"arrival_ticks\":5e0"); write(file,text); bad(file,"$.tasks[0].arrival_ticks");
    text = document().dump(); pos = text.find("\"c\":0"); text.replace(pos,5,"\"c\":0.1234567890123456789012345"); write(file,text);
    const auto precise = load_scenario(file);
    require(precise.scenario->adaptive().coefficient == 0.1234567890123456789012345L, "Real value narrowed through double");
    text = document().dump(); pos = text.find("\"id\":2");
    require(pos != std::string::npos,"Missing ID overflow fixture");
    text.replace(pos,6,"\"id\":18446744073709551616"); write(file,text); bad(file,"$.nodes[0].id");
    value = document(); value["delivery_trace"] = ""; bad_doc(value,"$.delivery_trace");
    value = document(); value["delivery_trace"] = std::string{"delivery.csv"}+std::string(1,'\0')+"hidden";
    bad_doc(value,"$.delivery_trace");
    value = document(); value["delivery_trace"] = "absent.csv"; bad(fixture(value),"line 1","absent.csv");
    bad(directory/"absent.json","$","absent.json");
    bad(fs::path{file.string()+std::string(1,'\0')+"hidden"},"$");
}
void csv_checks() {
    const auto invalid = [&](const std::string& data,std::string_view where) { bad(fixture(document(),data),where,"delivery.csv"); };
    invalid("", "line 1");
    invalid("node_id,heartbeat_seq,delay,lost\n","line 1");
    invalid("node_id,heartbeat_seq,delay_ticks,lost\n1,1,0\n","line 2");
    invalid(csv+"1,1,0,0\n","line 6, node_id/heartbeat_seq");
    invalid("node_id,heartbeat_seq,delay_ticks,lost\n99,1,0,0\n","line 2, node_id");
    invalid("node_id,heartbeat_seq,delay_ticks,lost\n1,3,0,0\n","line 2, heartbeat_seq");
    invalid("node_id,heartbeat_seq,delay_ticks,lost\n1,1,0,2\n","line 2, lost");
    for (const auto field : {"-1","1.0","1e0","true","18446744073709551616",""}) {
        invalid("node_id,heartbeat_seq,delay_ticks,lost\n1,1,"+std::string{field}+",0\n","line 2, delay_ticks");
    }
    invalid("node_id,heartbeat_seq,delay_ticks,lost\n1,1,9223372036854775808,0\n","delay_ticks");
    invalid("node_id,heartbeat_seq,delay_ticks,lost\n1,1,0,0\n","heartbeat_seq for node_id=1");
    const std::string crlf = "\xEF\xBB\xBFnode_id,heartbeat_seq,delay_ticks,lost\r\n 1 , 1 , 0 , 0 \r\n1,2,0,0\r\n2,1,0,0\r\n2,2,0,0";
    require(load_scenario(fixture(document(),crlf)).scenario->physical()->deliveries().size() == 4, "BOM/CRLF/spacing/no final newline");
}
void ranges() {
    auto value = document(); value["schema_version"] = 2; bad_doc(value,"$.schema_version");
    value = document(); value["tick_ns"] = 1; bad_doc(value,"$.tick_ns");
    for (const auto& bad_value : {Doc(0),Doc(-1),Doc(20.0),Doc(true),Doc(UINT64_C(9223372036854775808))}) {
        value = document(); value["horizon_ticks"] = bad_value; bad_doc(value,"$.horizon_ticks");
    }
    value = document(); value["heartbeat_period_ticks"] = 0; bad_doc(value,"$.heartbeat_period_ticks");
    value = document(); value["tau0_ticks"] = 9; bad_doc(value,"$.tau0_ticks");
    value = document(); value["kappa"] = 1; bad_doc(value,"$.kappa");
    value = document(); value["tau0_ticks"] = std::numeric_limits<std::int64_t>::max(); bad_doc(value,"$.kappa");
    value = document(); value["adaptive"]["min_intervals"] = 1; bad_doc(value,"$.adaptive.min_intervals");
    value = document(); value["adaptive"]["window"] = 1; bad_doc(value,"$.adaptive.window");
    value = document(); value["adaptive"]["c"] = -1; bad_doc(value,"$.adaptive.c");
    value = document(); value["nodes"][0]["id"] = 0; bad_doc(value,"$.nodes[0].id");
    value = document(); value["nodes"][1]["id"] = 2; bad_doc(value,"$.nodes[1].id");
    value = document(); value["nodes"][0]["performance_work_per_second"] = 0; bad_doc(value,"$.nodes[0].performance_work_per_second");
    value = document(); value["tasks"][1]["id"] = 2; bad_doc(value,"$.tasks[1].id");
    value = document(); value["tasks"][0]["arrival_ticks"] = 21; bad_doc(value,"$.tasks[0].arrival_ticks");
    value = document(); value["tasks"][0]["work"] = 0; bad_doc(value,"$.tasks[0].work");
    value = document(); value["tasks"][0]["work"] = 1e100; bad_doc(value,"$.tasks[0].work");
    value = document(); value["crashes"] = Doc::array({{{"node_id",99},{"at_ticks",0}}}); bad_doc(value,"$.crashes[0].node_id");
    value = document(); value["crashes"] = Doc::array({{{"node_id",1},{"at_ticks",21}}}); bad_doc(value,"$.crashes[0].at_ticks");
    value = document(); value["crashes"] = Doc::array({{{"node_id",1},{"at_ticks",0}},{{"node_id",1},{"at_ticks",2}}}); bad_doc(value,"$.crashes[1].node_id");
    value = document(); value["blackouts"] = Doc::array({{{"node_id",1},{"start_ticks",5},{"end_ticks",5}}}); bad_doc(value,"$.blackouts[0].end_ticks");
    value = document(); value["measurement_windows"][0]["end_ticks"] = 21; bad_doc(value,"$.measurement_windows[0].end_ticks");
    value = document(); value["measurement_windows"][0]["start_ticks"] = 20; bad_doc(value,"$.measurement_windows[0].end_ticks");
}
struct Observations : simulation::ObservationSink {
    std::vector<std::pair<SimTime,HeartbeatMessage>> received;
    void on_result(SimTime,ResultMessage) override {}
    void on_heartbeat(SimTime time,HeartbeatMessage message) override { received.emplace_back(time,message); }
};
void immutable() {
    const auto file = fixture(document());
    const auto original_json = read(file); const auto original_csv = read(directory/"delivery.csv");
    const auto loaded = load_scenario(file);
    const auto run = [&]() {
        engine::EventLoop loop{loaded.scenario->physical()->horizon()};
        Observations observer;
        simulation::PhysicalEnvironment world{loop,loaded.scenario->physical(),observer}; world.initialize();
        require(world.nodes().at(NodeId{1}).sent_heartbeats == 0, "Variant reused mutable physical state");
        static_cast<void>(loop.run()); world.validate_invariants(); return observer.received;
    };
    const auto first = run(); const auto second = run();
    require(first == second && first.size() == 4 && loaded.scenario->physical()->delivery(NodeId{1},HeartbeatSeq{1}).delay == SimDuration{0},
            "Shared loaded input changed between states");
    require(read(file) == original_json && read(directory/"delivery.csv") == original_csv, "Loader/run modified input files");
    const auto again = load_scenario(file);
    require(again.scenario->tasks()[0] == loaded.scenario->tasks()[0] && again.scenario.get() != loaded.scenario.get(),
            "Repeated loading reused state or changed data");
}
void examples() {
    const fs::path root{TELEMETRY_SOURCE_DIR};
    for (const auto name : {"baseline","crash","blackout"}) {
        const auto loaded = load_scenario(root/"examples"/name/"scenario.json");
        require(loaded.scenario->tasks().size() == 4 && loaded.scenario->physical()->nodes().size() == 2,
                "Example load failed");
        require(loaded.scenario->physical()->deliveries().size() == (std::string_view{name} == "baseline" ? 4U : 16U),
                "Example trace incomplete");
    }
    const auto baseline = load_scenario(root/"examples/baseline/scenario.json");
    require(baseline.scenario->tasks()[0].work == 4 && baseline.scenario->physical()->horizon() == SimTime{20}, "Baseline differs from manual plan");
    const auto crash = load_scenario(root/"examples/crash/scenario.json");
    require(crash.scenario->physical()->crashes()[0].at == SimTime{3}, "Crash example instant");
    const auto blackout = load_scenario(root/"examples/blackout/scenario.json");
    require(blackout.scenario->physical()->blocked(NodeId{1},SimTime{10}) &&
            !blackout.scenario->physical()->blocked(NodeId{1},SimTime{21}), "Blackout example half-open boundaries");
}
}  // namespace
int main(int argc,char* argv[]) {
    try {
        if (argc != 2) { throw std::runtime_error("Expected group"); }
        const std::string_view group{argv[1]};
        directory = fs::current_path()/("scenario-fixtures-"+std::string{group});
        if (group == "valid") { valid(); }
        else if (group == "json") { json_checks(); }
        else if (group == "csv") { csv_checks(); }
        else if (group == "ranges") { ranges(); }
        else if (group == "immutable") { immutable(); }
        else if (group == "examples") { examples(); }
        else { throw std::runtime_error("Unknown group"); }
        std::cout << group << " checks passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "Scenario test failed: " << error.what() << '\n'; return 1;
    }
}
