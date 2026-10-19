#include "telemetry/io/build_info.hpp"

#include "telemetry/common/build_config.hpp"
#include <nlohmann/json.hpp>

namespace telemetry::io {
std::string build_info_json() {
    const nlohmann::json info{
        {"program", "telemetry_sim"},
        {"version", build::version},
        {"implementation_stage", build::implementation_stage},
        {"simulation_available", false},
        {"compiler", {{"id", build::compiler_id}, {"version", build::compiler_version}}},
        {"cplusplus", __cplusplus},
        {"nlohmann_json", {
            {"major", NLOHMANN_JSON_VERSION_MAJOR},
            {"minor", NLOHMANN_JSON_VERSION_MINOR},
            {"patch", NLOHMANN_JSON_VERSION_PATCH}}}};
    return info.dump();
}
}  // namespace telemetry::io
