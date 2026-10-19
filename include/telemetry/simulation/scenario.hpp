#pragma once

#include "telemetry/core/detector.hpp"
#include "telemetry/simulation/physical_trace.hpp"
#include <memory>

namespace telemetry::simulation {
struct MeasurementWindow {
    SimTime start;
    SimTime end;
    bool operator==(const MeasurementWindow&) const = default;
};
class Scenario {
public:
    Scenario(std::shared_ptr<const PhysicalTrace> physical, core::TimeoutParameters timeout,
             core::AdaptiveTimeout adaptive, std::vector<TaskDescription> tasks,
             std::vector<MeasurementWindow> windows);
    Scenario(const Scenario&) = default;
    Scenario& operator=(const Scenario&) = delete;
    Scenario& operator=(Scenario&&) = delete;
    [[nodiscard]] const std::shared_ptr<const PhysicalTrace>& physical() const noexcept { return physical_; }
    [[nodiscard]] const core::TimeoutParameters& timeout() const noexcept { return timeout_; }
    [[nodiscard]] const core::AdaptiveTimeout& adaptive() const noexcept { return adaptive_; }
    [[nodiscard]] std::span<const TaskDescription> tasks() const noexcept { return tasks_; }
    [[nodiscard]] std::span<const MeasurementWindow> measurement_windows() const noexcept { return windows_; }
private:
    std::shared_ptr<const PhysicalTrace> physical_;
    core::TimeoutParameters timeout_;
    core::AdaptiveTimeout adaptive_;
    std::vector<TaskDescription> tasks_;
    std::vector<MeasurementWindow> windows_;
};
}  // namespace telemetry::simulation
