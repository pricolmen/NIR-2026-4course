#pragma once

#include <string>

namespace telemetry::io {
// Build metadata only; not a simulation manifest or scenario validator.
[[nodiscard]] std::string build_info_json();
}  // namespace telemetry::io
