#include "telemetry/common/build_config.hpp"
#include "telemetry/io/build_info.hpp"

#include <exception>
#include <iostream>
#include <string_view>

namespace {
constexpr std::string_view usage =
    "Usage: telemetry_sim --help | --version | --build-info\n"
    "Implementation stage 8: scheduling, logical tasks and build metadata.\n"
    "Simulation commands will be implemented in later stages.\n";
}

int main(int argc, char* argv[]) {
    try {
        if (argc == 1) {
            std::cerr << usage;
            return 2;
        }
        if (argc != 2) {
            std::cerr << "Unknown or unsupported arguments.\n" << usage;
            return 2;
        }
        const std::string_view argument{argv[1]};
        if (argument == "--help") {
            std::cout << usage;
        } else if (argument == "--version") {
            std::cout << "telemetry_sim " << telemetry::build::version << '\n';
        } else if (argument == "--build-info") {
            std::cout << telemetry::io::build_info_json() << '\n';
        } else {
            std::cerr << "Unknown or unsupported argument: " << argument << '\n' << usage;
            return 2;
        }
        std::cout.flush();
        if (!std::cout) {
            std::cerr << "Failed to write command output.\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
