#pragma once

#include "telemetry/simulation/simulation.hpp"
#include <iosfwd>

namespace telemetry::io {
class JournalWriter final : public simulation::SimulationAuditSink {
public:
    explicit JournalWriter(std::ostream& output);
    void on_event(const engine::EventKey& key,const simulation::SimulationEvent& event) override;
    void complete(engine::RunResult result);
private:
    void line(const std::string& json);
    std::ostream& output_;
    std::uint64_t records_ = 0;
    bool complete_ = false,failed_ = false;
};
// Streams one line at a time. Rejects missing/duplicate completion, invalid
// schema/types/enums, record gaps and trailing records; errors include line no.
[[nodiscard]] engine::RunResult read_journal(std::istream& input,simulation::SimulationAuditSink& sink);
}  // namespace telemetry::io
