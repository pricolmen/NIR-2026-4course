#include "telemetry/simulation/scenario.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace telemetry::simulation {
Scenario::Scenario(std::shared_ptr<const PhysicalTrace> physical, core::TimeoutParameters timeout,
                   core::AdaptiveTimeout adaptive,std::vector<TaskDescription> tasks,
                   std::vector<MeasurementWindow> windows)
    : physical_(std::move(physical)),timeout_(timeout),adaptive_(adaptive),tasks_(std::move(tasks)),windows_(std::move(windows)) {
    if (!physical_ || physical_->horizon() == SimTime{} || timeout_.heartbeat_period != physical_->heartbeat_period()) {
        throw std::invalid_argument("Scenario requires positive matching horizon/period");
    }
    const core::Detector validation{NodeId{1},timeout_,adaptive_};
    std::set<TaskId> ids;
    const auto nodes = physical_->nodes();
    if (nodes.empty()) { throw std::invalid_argument("Scenario requires at least one node"); }
    const auto [slow,fast] = std::minmax_element(nodes.begin(),nodes.end(),[](const auto& a,const auto& b) {
        return a.performance < b.performance;
    });
    for (const auto& task : tasks_) {
        if (!ids.insert(task.id).second || task.arrival > physical_->horizon() || !std::isfinite(task.work) || task.work <= 0) {
            throw std::invalid_argument("Invalid or duplicate scenario task");
        }
        static_cast<void>(processing_duration(task.work,slow->performance));
        static_cast<void>(processing_duration(task.work,fast->performance));
    }
    std::sort(tasks_.begin(),tasks_.end(),[](const auto& a,const auto& b) {
        return std::tie(a.arrival,a.id) < std::tie(b.arrival,b.id);
    });
    for (const auto& window : windows_) {
        if (window.start >= window.end || window.end > physical_->horizon()) { throw std::invalid_argument("Invalid measurement window"); }
    }
}
}  // namespace telemetry::simulation
