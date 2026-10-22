#pragma once
#include "telemetry/io/run_output.hpp"
#include <ostream>
#include <string_view>

namespace telemetry::console {
std::string_view variant_name(simulation::Variant variant);
void configuration(std::ostream& out,const simulation::Scenario& scenario);
void result(std::ostream& out,std::string_view name,const metrics::Summary& summary);
void comparison(std::ostream& out,const io::Comparison& summaries);
}  // namespace telemetry::console
