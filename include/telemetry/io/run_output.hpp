#pragma once

#include "telemetry/io/scenario_loader.hpp"
#include "telemetry/metrics/analysis.hpp"

namespace telemetry::io {
// Creates a new output directory (or uses an empty one); never overwrites
// existing results. Saves input snapshots so later analysis survives a move.
// On error the manifest stays incomplete and no successful run is returned.
[[nodiscard]] metrics::Analysis write_run(const std::filesystem::path& scenario_file,
    simulation::Variant variant,const std::filesystem::path& output_directory);
// Runs all four variants on one frozen shared Scenario and records comparison.csv.
using Comparison = std::array<metrics::Summary,4>;
[[nodiscard]] Comparison write_comparison(const std::filesystem::path& scenario_file,const std::filesystem::path& output_directory);
// Loads snapshots and verifies manifest/checksums, then independently streams
// events.jsonl through Analyzer. Does not run Simulation or trust metrics.json.
[[nodiscard]] metrics::Analysis analyze_run(const std::filesystem::path& output_directory);
// Rewrites derived CSV/metrics only after successful independent analysis.
void recalculate_reports(const std::filesystem::path& output_directory);
}  // namespace telemetry::io
