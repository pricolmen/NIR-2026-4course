#pragma once

#include "telemetry/simulation/scenario.hpp"
#include <filesystem>
#include <stdexcept>
#include <string>

namespace telemetry::io {
class InputError : public std::runtime_error {
    static std::string display_file(const std::filesystem::path& file) {
        std::string result;
        for (const auto character : file.string()) {
            if (character == '\0') { result += "\\0"; }
            else { result += character; }
        }
        return result;
    }
public:
    InputError(const std::filesystem::path& file,const std::string& location,const std::string& detail)
        : std::runtime_error(display_file(file)+": "+location+": "+detail) {}
};
struct LoadedScenario {
    std::shared_ptr<const simulation::Scenario> scenario;
    std::filesystem::path scenario_file;
    std::filesystem::path delivery_file;
};
// Paths in the returned metadata are absolute. Relative delivery_trace is
// resolved against the JSON file's parent, independently of process cwd.
[[nodiscard]] LoadedScenario load_scenario(const std::filesystem::path& file);
}  // namespace telemetry::io
