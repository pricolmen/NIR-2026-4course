#include "telemetry/common/build_config.hpp"
#include "telemetry/io/build_info.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}
}

int main() {
    try {
        require(__cplusplus >= 202002L, "C++20 is required");
        const auto info = nlohmann::json::parse(telemetry::io::build_info_json());
        require(info.at("program") == "telemetry_sim", "Wrong program metadata");
        require(info.at("version").get<std::string>() == telemetry::build::version,
                "Version differs from CMake configuration");
        require(info.at("compiler").at("id").is_string(), "Missing compiler metadata");
        require(info.at("implementation_stage") == telemetry::build::implementation_stage, "Unexpected implementation stage");
        require(info.at("simulation_available") == true, "CLI simulation must be available");
        require(info.at("nlohmann_json") == nlohmann::json{
                    {"major", 3}, {"minor", 12}, {"patch", 0}}, "Unexpected JSON release");

        // Verify the selected JSON integration preserves integers beyond double's
        // exact range; this is not yet a scenario/time-field validator (stage 9).
        const auto values = nlohmann::json::parse(
            R"({"ticks":9223372036854775807,"large_id":9007199254740993,"fraction":1.5})");
        require(values.at("ticks").is_number_integer(), "Integer ticks lost their type");
        require(values.at("ticks").get<std::int64_t>() == std::numeric_limits<std::int64_t>::max(),
                "Integer ticks lost precision");
        require(values.at("large_id").get<std::uint64_t>() == UINT64_C(9007199254740993),
                "Large ID lost precision");
        require(values.at("fraction").is_number_float(), "Fraction must remain distinguishable");
        require(nlohmann::json::parse(values.dump()) == values, "JSON round-trip changed values");

        bool malformed_rejected = false;
        try {
            const auto invalid = nlohmann::json::parse("{bad}");
            static_cast<void>(invalid);
        } catch (const nlohmann::json::parse_error&) {
            malformed_rejected = true;
        }
        require(malformed_rejected, "Malformed JSON was accepted");
        std::cout << "Build metadata and JSON integration checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Build smoke test failed: " << error.what() << '\n';
        return 1;
    }
}
