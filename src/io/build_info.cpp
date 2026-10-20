#include "telemetry/io/build_info.hpp"

#include "telemetry/common/build_config.hpp"
#include <nlohmann/json.hpp>

namespace telemetry::io {
std::string build_info_json() {
    nlohmann::json info{
        {"program", "telemetry_sim"},
        {"version", build::version},
        {"implementation_stage", build::implementation_stage},
        {"simulation_available", true},
        {"compiler", {{"id", build::compiler_id}, {"version", build::compiler_version}}},
        {"cplusplus", __cplusplus},
        {"nlohmann_json", {
            {"major", NLOHMANN_JSON_VERSION_MAJOR},
            {"minor", NLOHMANN_JSON_VERSION_MINOR},
            {"patch", NLOHMANN_JSON_VERSION_PATCH}}}};
#if defined(_LIBCPP_VERSION)
    info["standard_library"] = {{"name","libc++"},{"version",_LIBCPP_VERSION}};
#elif defined(__GLIBCXX__)
    info["standard_library"] = {{"name","libstdc++"},{"version",__GLIBCXX__}};
#else
    info["standard_library"] = {{"name","unknown"}};
#endif
#if defined(__APPLE__)
    info["platform"] = "macOS";
#elif defined(__linux__)
    info["platform"] = "Linux";
#elif defined(_WIN32)
    info["platform"] = "Windows";
#else
    info["platform"] = "unknown";
#endif
    return info.dump();
}
}  // namespace telemetry::io
