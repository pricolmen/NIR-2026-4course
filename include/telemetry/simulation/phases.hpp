#pragma once
#include "telemetry/engine/event_loop.hpp"

namespace telemetry::simulation::phases {
inline constexpr engine::Phase crash{1};
inline constexpr engine::Phase completion{2};
inline constexpr engine::Phase heartbeat_send{3};
inline constexpr engine::Phase heartbeat_receive{4};
inline constexpr engine::Phase timeout{5};
inline constexpr engine::Phase reconfiguration{6};
inline constexpr engine::Phase arrival{7};
inline constexpr engine::Phase dispatch{8};
}  // namespace telemetry::simulation::phases
