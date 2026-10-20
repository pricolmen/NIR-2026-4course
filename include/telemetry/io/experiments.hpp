#pragma once
#include "telemetry/io/run_output.hpp"

namespace telemetry::io {
struct GenerationPlan {
    std::filesystem::path source;
    std::string source_checksum;
    std::uint64_t seed;
    SimTime horizon;
    simulation::PhysicalInput physical;
    core::TimeoutParameters timeout;
    core::AdaptiveTimeout adaptive;
    std::vector<simulation::MeasurementWindow> windows;
    std::uint64_t task_count;
    SimTime first_arrival;
    SimDuration task_period,min_delay,max_delay;
    long double min_work,max_work,loss_probability;
};
[[nodiscard]] GenerationPlan load_generation_plan(const std::filesystem::path& config);
void write_generated_scenario(const GenerationPlan& plan,const std::filesystem::path& output_directory);
struct BatchEntry {
    std::string scenario_id,repeat_id;
    std::filesystem::path scenario_file,delivery_file;
};
// Validates all referenced inputs before a batch starts; relative paths use
// the manifest directory. IDs are labels, never filesystem path components.
[[nodiscard]] std::vector<BatchEntry> load_batch(const std::filesystem::path& manifest);
void write_batch(const std::vector<BatchEntry>& entries,const std::filesystem::path& output_directory);
}  // namespace telemetry::io
