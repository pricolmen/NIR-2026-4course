#include "telemetry/common/build_config.hpp"
#include "telemetry/io/build_info.hpp"
#include "telemetry/io/experiments.hpp"
#include "telemetry/io/output_directory.hpp"

#include <exception>
#include <iostream>
#include <map>
#include <set>
#include <string_view>

namespace {
using namespace telemetry;
namespace fs = std::filesystem;
constexpr std::string_view usage =
    "Usage: telemetry_sim <command> [options]\n"
    "Implementation stage 15: CLI, saved runs and four-variant comparisons.\n"
    "  validate --scenario FILE\n"
    "  run --scenario FILE --detector fixed|adaptive --scheduler rr|ll --out DIR [--overwrite]\n"
    "  compare --scenario FILE --out DIR [--overwrite]\n"
    "  metrics --run-dir DIR\n"
    "  generate --config FILE --out DIR [--overwrite]\n"
    "  batch --manifest FILE --out DIR [--overwrite]\n"
    "  --help | --version | --build-info\n"
    "Existing outputs are preserved without --overwrite; replacement retains a sibling backup.\n";
struct UsageError : std::runtime_error { using std::runtime_error::runtime_error; };
struct Options {
    std::map<std::string,std::string> values; bool overwrite = false;
    const std::string& at(const std::string& key) const {
        const auto it = values.find(key); if (it == values.end()) { throw UsageError("Missing required option: "+key); } return it->second;
    }
};
Options parse(int argc,char** argv,const std::set<std::string>& allowed,bool writes) {
    Options result;
    for (int i = 2; i < argc; ++i) {
        const std::string key{argv[i]};
        if (key == "--overwrite" && writes) {
            if (result.overwrite) { throw UsageError("Duplicate option: "+key); } result.overwrite = true; continue;
        }
        if (!allowed.contains(key)) { throw UsageError("Unknown or unsupported option: "+key); }
        if (++i >= argc || std::string_view{argv[i]}.starts_with("--") || std::string_view{argv[i]}.empty()) { throw UsageError("Missing value for "+key); }
        if (!result.values.emplace(key,argv[i]).second) { throw UsageError("Duplicate option: "+key); }
    }
    for (const auto& key : allowed) { static_cast<void>(result.at(key)); } return result;
}
simulation::Variant variant(const Options& options) {
    const auto& detector = options.at("--detector"); const auto& scheduler = options.at("--scheduler");
    if (detector != "fixed" && detector != "adaptive") { throw UsageError("--detector must be fixed or adaptive"); }
    if (scheduler != "rr" && scheduler != "ll") { throw UsageError("--scheduler must be rr or ll"); }
    if (detector == "fixed") { return scheduler == "rr" ? simulation::Variant::fixed_rr : simulation::Variant::fixed_ll; }
    return scheduler == "rr" ? simulation::Variant::adaptive_rr : simulation::Variant::adaptive_ll;
}
void publish(io::OutputDirectory& output,const std::string& dir) {
    const auto backup = output.commit(); std::cout << "Saved: " << dir << '\n';
    if (!backup.empty()) { std::cout << "Previous output: " << backup.string() << '\n'; }
}
void command(int argc,char** argv) {
    if (argc < 2) { throw UsageError("Missing command"); }
    const std::string name{argv[1]};
    const std::set<std::string> commands{"validate","run","compare","metrics","generate","batch"};
    if (argc == 2 && name == "--help") { std::cout << usage; return; }
    if (argc == 2 && name == "--version") { std::cout << "telemetry_sim " << build::version << '\n'; return; }
    if (argc == 2 && name == "--build-info") { std::cout << io::build_info_json() << '\n'; return; }
    if (!commands.contains(name)) { throw UsageError("Unknown or unsupported command/arguments: "+name); }
    if (argc == 3 && std::string_view{argv[2]} == "--help") { std::cout << usage; return; }
    if (name == "validate") {
        const auto options = parse(argc,argv,{"--scenario"},false); const auto loaded = io::load_scenario(options.at("--scenario"));
        std::cout << "Valid scenario: nodes=" << loaded.scenario->physical()->nodes().size() << " tasks=" << loaded.scenario->tasks().size()
            << " horizon_ticks=" << loaded.scenario->physical()->horizon().ticks() << '\n';
    } else if (name == "metrics") {
        const auto options = parse(argc,argv,{"--run-dir"},false); io::recalculate_reports(options.at("--run-dir"));
        std::cout << "Recalculated reports from saved journal: " << options.at("--run-dir") << '\n';
    } else if (name == "run" || name == "compare") {
        const auto options = parse(argc,argv,name == "run" ? std::set<std::string>{"--scenario","--out","--detector","--scheduler"} : std::set<std::string>{"--scenario","--out"},true);
        const auto selected = name == "run" ? variant(options) : simulation::Variant::fixed_rr;
        const auto loaded = io::load_scenario(options.at("--scenario"));
        io::OutputDirectory output{options.at("--out"),options.overwrite,{loaded.scenario_file,loaded.delivery_file}};
        if (name == "run") { static_cast<void>(io::write_run(loaded.scenario_file,selected,output.workspace())); }
        else { static_cast<void>(io::write_comparison(loaded.scenario_file,output.workspace())); }
        publish(output,options.at("--out"));
    } else if (name == "generate") {
        const auto options = parse(argc,argv,{"--config","--out"},true); const auto plan = io::load_generation_plan(options.at("--config"));
        io::OutputDirectory output{options.at("--out"),options.overwrite,{plan.source}};
        io::write_generated_scenario(plan,output.workspace()); publish(output,options.at("--out"));
    } else {
        const auto options = parse(argc,argv,{"--manifest","--out"},true); const auto entries = io::load_batch(options.at("--manifest"));
        std::vector<fs::path> inputs{options.at("--manifest")};
        for (const auto& entry : entries) { inputs.push_back(entry.scenario_file); inputs.push_back(entry.delivery_file); }
        io::OutputDirectory output{options.at("--out"),options.overwrite,inputs};
        io::write_batch(entries,output.workspace()); publish(output,options.at("--out"));
    }
}
}
int main(int argc,char** argv) {
    try {
        command(argc,argv); std::cout.flush(); if (!std::cout) { throw std::runtime_error("Failed to write command output"); } return 0;
    } catch (const UsageError& error) { std::cerr << error.what() << '\n' << usage; return 2; }
    catch (const std::exception& error) { std::cerr << "Error: " << error.what() << '\n'; return 1; }
}
