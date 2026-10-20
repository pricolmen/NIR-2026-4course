#include "telemetry/io/journal.hpp"
#include "json_support.hpp"

#include <istream>
#include <ostream>
#include <type_traits>

namespace telemetry::io {
namespace {
using namespace detail;
using namespace simulation;
Json task(TaskDescription value) { return {{"id",value.id.value()},{"arrival_ticks",value.arrival.ticks()},{"work_decimal",decimal(value.work)}}; }
TaskDescription task(const Json& value) { return {TaskId{integer(value.at("id"))},SimTime{integer(value.at("arrival_ticks"))},real(value.at("work_decimal"))}; }
Json command(AssignmentCommand value) { return {{"task",task(value.task)},{"node_id",value.node.value()},{"attempt_no",value.attempt.value()}}; }
AssignmentCommand command(const Json& value) { return {task(value.at("task")),NodeId{integer(value.at("node_id"))},AttemptNo{integer(value.at("attempt_no"))}}; }
Json result(ResultMessage value) { return {{"task_id",value.task.value()},{"node_id",value.node.value()},{"attempt_no",value.attempt.value()}}; }
ResultMessage result(const Json& value) { return {TaskId{integer(value.at("task_id"))},AttemptNo{integer(value.at("attempt_no"))},NodeId{integer(value.at("node_id"))}}; }
Json heartbeat(HeartbeatMessage value) { return {{"node_id",value.node.value()},{"heartbeat_seq",value.sequence.value()}}; }
HeartbeatMessage heartbeat(const Json& value) { return {NodeId{integer(value.at("node_id"))},HeartbeatSeq{integer(value.at("heartbeat_seq"))}}; }
Json timer(core::TimeoutRequest value) {
    return {{"node_id",value.node.value()},{"at_ticks",value.at.ticks()},{"kind",name(value.kind,timeout_kinds)},{"generation",value.generation}};
}
core::TimeoutRequest timer(const Json& value) {
    return {NodeId{integer(value.at("node_id"))},SimTime{integer(value.at("at_ticks"))},enumeration<core::TimeoutKind>(value.at("kind"),timeout_kinds),integer(value.at("generation"))};
}
Json thresholds(const core::DetectorThresholds& value) {
    return {{"raw_suspect_ticks_decimal",decimal(value.durations.raw_suspect_ticks)},{"raw_failed_ticks_decimal",decimal(value.durations.raw_failed_ticks)},
        {"suspect_ticks",value.durations.suspect.ticks()},{"failed_ticks",value.durations.failed.ticks()},{"interval_count",value.statistics.count},
        {"mean_ticks_decimal",optional_real(value.statistics.mean_ticks)},{"sample_std_ticks_decimal",optional_real(value.statistics.sample_std_ticks)},{"warmed_up",value.warmed_up}};
}
core::DetectorThresholds thresholds(const Json& value) {
    const auto count = integer(value.at("interval_count")); require(count <= std::numeric_limits<std::size_t>::max());
    const auto rounded = round_thresholds(real(value.at("raw_suspect_ticks_decimal")),real(value.at("raw_failed_ticks_decimal")));
    require(rounded.suspect == SimDuration{integer(value.at("suspect_ticks"))} && rounded.failed == SimDuration{integer(value.at("failed_ticks"))});
    return {rounded,{static_cast<std::size_t>(count),read_real(value.at("mean_ticks_decimal")),read_real(value.at("sample_std_ticks_decimal"))},boolean(value.at("warmed_up"))};
}
Json encode(const SimulationEvent& event) {
    Json output;
    const auto nested = [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T,core::TaskArrivedEvent>) { output = {{"kind","task_arrived"},{"task",task(value.task)}}; }
        else if constexpr (std::is_same_v<T,core::TaskAssignedEvent> || std::is_same_v<T,PhysicalAssigned>) {
            output = {{"kind",std::is_same_v<T,PhysicalAssigned> ? "physical_assigned" : "task_assigned"},{"assignment",command(value.assignment)}};
        } else if constexpr (std::is_same_v<T,core::AssignmentRevokedEvent>) { output = {{"kind","assignment_revoked"},{"attempt",result(value.assignment)}}; }
        else if constexpr (std::is_same_v<T,core::ResultEvent>) { output = {{"kind","result"},{"attempt",result(value.result)},{"decision",name(value.decision,result_decisions)}}; }
        else if constexpr (std::is_same_v<T,core::ReconfigurationEvent>) {
            output = {{"kind","reconfiguration"},{"alive",Json::array()},{"newly_failed",Json::array()},{"revoked",Json::array()}};
            for (auto id : value.alive) { output["alive"].push_back(id.value()); }
            for (auto id : value.newly_failed) { output["newly_failed"].push_back(id.value()); }
            for (auto msg : value.revoked) { output["revoked"].push_back(result(msg)); }
        } else if constexpr (std::is_same_v<T,PhysicalStarted>) {
            output = {{"kind","physical_started"},{"attempt",result(value.attempt)},{"duration_ticks",value.duration.ticks()},{"completion_due_ticks",time(value.completion_due)},{"token",value.token}};
        } else if constexpr (std::is_same_v<T,PhysicalFinished>) { output = {{"kind","physical_finished"},{"attempt",result(value.attempt)}}; }
        else if constexpr (std::is_same_v<T,PhysicalCrash>) { output = {{"kind","crash"},{"node_id",value.node.value()},{"interrupted",value.interrupted ? result(*value.interrupted) : Json(nullptr)}}; }
        else if constexpr (std::is_same_v<T,PhysicalCompletionSkipped>) {
            output = {{"kind","completion_skipped"},{"attempt",result(value.attempt)},{"scheduled_token",value.scheduled_token},{"current_token",value.current_token},{"reason",name(value.reason,skip_reasons)}};
        } else if constexpr (std::is_same_v<T,PhysicalHeartbeatSent>) {
            output = {{"kind","heartbeat_sent"},{"message",heartbeat(value.message)},{"delay_ticks",value.delay.ticks()},{"disposition",name(value.disposition,dispositions)},{"receive_at_ticks",time(value.receive_at)}};
        } else if constexpr (std::is_same_v<T,PhysicalHeartbeatDelivered>) {
            output = {{"kind","heartbeat_delivered"},{"message",heartbeat(value.message)},{"sent_at_ticks",value.sent_at.ticks()}};
        } else if constexpr (std::is_same_v<T,PhysicalSendSkipped>) { output = {{"kind","send_skipped"},{"message",heartbeat(value.message)}}; }
    };
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T,core::AuditEvent> || std::is_same_v<T,PhysicalEvent>) { std::visit(nested,value); }
        else if constexpr (std::is_same_v<T,DispatchBatch>) { output = {{"kind","dispatch"},{"commands",value.commands}}; }
        else {
            output = {{"kind",std::is_same_v<T,HeartbeatObserved> ? "heartbeat_observed" : "timeout_observed"},
                {"before",name(value.update.before,statuses)},{"after",name(value.update.after,statuses)},{"thresholds",thresholds(value.thresholds)}};
            if constexpr (std::is_same_v<T,HeartbeatObserved>) {
                output["message"] = heartbeat(value.message); output["decision"] = name(value.update.decision,heartbeat_decisions);
                output["generation"] = value.generation; output["contact_ticks"] = value.contact.ticks(); output["timers"] = Json::array();
                for (auto request : value.update.timers) { output["timers"].push_back(timer(request)); }
            } else { output["request"] = timer(value.request); output["decision"] = name(value.update.decision,timeout_decisions); }
        }
    },event); return output;
}
SimulationEvent decode(const Json& value,SimTime at) {
    const auto kind = value.at("kind").get<std::string>();
    if (kind == "task_arrived") { return core::AuditEvent{core::TaskArrivedEvent{at,task(value.at("task"))}}; }
    if (kind == "task_assigned") { return core::AuditEvent{core::TaskAssignedEvent{at,command(value.at("assignment"))}}; }
    if (kind == "assignment_revoked") { return core::AuditEvent{core::AssignmentRevokedEvent{at,result(value.at("attempt"))}}; }
    if (kind == "result") { return core::AuditEvent{core::ResultEvent{at,result(value.at("attempt")),enumeration<core::ResultDecision>(value.at("decision"),result_decisions)}}; }
    if (kind == "reconfiguration") {
        core::ReconfigurationEvent event{at,{},{},{}};
        for (const auto& name_value : {"alive","newly_failed","revoked"}) { require(value.at(name_value).is_array()); }
        for (const auto& id : value.at("alive")) { event.alive.emplace_back(integer(id)); }
        for (const auto& id : value.at("newly_failed")) { event.newly_failed.emplace_back(integer(id)); }
        for (const auto& msg : value.at("revoked")) { event.revoked.push_back(result(msg)); }
        return core::AuditEvent{std::move(event)};
    }
    if (kind == "physical_assigned") { return PhysicalEvent{PhysicalAssigned{at,command(value.at("assignment"))}}; }
    if (kind == "physical_started") { return PhysicalEvent{PhysicalStarted{at,result(value.at("attempt")),SimDuration{integer(value.at("duration_ticks"))},read_time(value.at("completion_due_ticks")),integer(value.at("token"))}}; }
    if (kind == "physical_finished") { return PhysicalEvent{PhysicalFinished{at,result(value.at("attempt"))}}; }
    if (kind == "crash") { return PhysicalEvent{PhysicalCrash{at,NodeId{integer(value.at("node_id"))},value.at("interrupted").is_null() ? std::nullopt : std::optional<ResultMessage>{result(value.at("interrupted"))}}}; }
    if (kind == "completion_skipped") { return PhysicalEvent{PhysicalCompletionSkipped{at,result(value.at("attempt")),integer(value.at("scheduled_token")),integer(value.at("current_token")),enumeration<CompletionSkipReason>(value.at("reason"),skip_reasons)}}; }
    if (kind == "heartbeat_sent") { return PhysicalEvent{PhysicalHeartbeatSent{at,heartbeat(value.at("message")),SimDuration{integer(value.at("delay_ticks"))},enumeration<DeliveryDisposition>(value.at("disposition"),dispositions),read_time(value.at("receive_at_ticks"))}}; }
    if (kind == "heartbeat_delivered") { return PhysicalEvent{PhysicalHeartbeatDelivered{at,SimTime{integer(value.at("sent_at_ticks"))},heartbeat(value.at("message"))}}; }
    if (kind == "send_skipped") { return PhysicalEvent{PhysicalSendSkipped{at,heartbeat(value.at("message"))}}; }
    if (kind == "dispatch") { const auto count = integer(value.at("commands")); require(count <= std::numeric_limits<std::size_t>::max()); return DispatchBatch{at,static_cast<std::size_t>(count)}; }
    if (kind == "heartbeat_observed" || kind == "timeout_observed") {
        const auto before = enumeration<core::NodeStatus>(value.at("before"),statuses),after = enumeration<core::NodeStatus>(value.at("after"),statuses);
        if (kind == "heartbeat_observed") {
            std::vector<core::TimeoutRequest> timers; require(value.at("timers").is_array());
            for (const auto& request : value.at("timers")) { timers.push_back(timer(request)); }
            return HeartbeatObserved{at,heartbeat(value.at("message")),{enumeration<core::HeartbeatDecision>(value.at("decision"),heartbeat_decisions),before,after,std::move(timers)},
                integer(value.at("generation")),SimTime{integer(value.at("contact_ticks"))},thresholds(value.at("thresholds"))};
        }
        return TimeoutObserved{at,timer(value.at("request")),{enumeration<core::TimeoutDecision>(value.at("decision"),timeout_decisions),before,after},thresholds(value.at("thresholds"))};
    }
    throw std::invalid_argument("Unknown journal event kind");
}
Json header(std::uint64_t record,std::string kind,SimTime at) {
    return {{"schema_version",1},{"record_no",record},{"time_ticks",at.ticks()},{"phase",0},{"kind",std::move(kind)}};
}
}
JournalWriter::JournalWriter(std::ostream& output) : output_(output) { line(header(1,"run_started",SimTime{}).dump()); }
void JournalWriter::line(const std::string& json) {
    if (complete_ || failed_) { throw std::logic_error("Journal is closed"); }
    try { const auto next = checked_increment(records_); output_ << json << '\n'; if (!output_) { throw std::runtime_error("Journal write failed"); } records_ = next; }
    catch (...) { failed_ = true; throw; }
}
void JournalWriter::on_event(const engine::EventKey& key,const SimulationEvent& event) {
    auto row = encode(event); row["schema_version"] = 1; row["record_no"] = checked_increment(records_);
    row["time_ticks"] = key.at.ticks(); row["phase"] = key.phase.value(); row["tie_key"] = key.tie_key;
    row["action_sequence"] = key.insertion_sequence; line(row.dump());
}
void JournalWriter::complete(engine::RunResult result_value) {
    auto row = header(checked_increment(records_),"run_completed",result_value.reached); row["executed_events"] = result_value.executed;
    line(row.dump()); output_.flush(); if (!output_) { failed_ = true; throw std::runtime_error("Journal flush failed"); } complete_ = true;
}
engine::RunResult read_journal(std::istream& input,SimulationAuditSink& sink) {
    std::string text; std::uint64_t number = 0; std::optional<engine::RunResult> completed;
    while (std::getline(input,text)) {
        number = checked_increment(number);
        try {
            require(!completed); auto row = parse(text); require(row.is_object() && integer(row.at("schema_version")) == 1 && integer(row.at("record_no")) == number);
            static_cast<void>(integer(row.at("phase")));
            const auto kind = row.at("kind").get<std::string>(); const SimTime at{integer(row.at("time_ticks"))};
            if (number == 1) { require(row == header(1,"run_started",SimTime{})); continue; }
            if (kind == "run_completed") {
                const auto executed = integer(row.at("executed_events")); auto expected = header(number,kind,at); expected["executed_events"] = executed;
                require(row == expected); completed = engine::RunResult{at,executed}; continue;
            }
            const engine::Phase phase{integer(row.at("phase"))}; require(phase.value() <= 8);
            const auto& ties = row.at("tie_key"); require(ties.is_array() && ties.size() == 4); engine::TieKey tie{};
            for (std::size_t i = 0; i < tie.size(); ++i) { tie[i] = integer(ties[i]); }
            const auto sequence = integer(row.at("action_sequence")); require(sequence > 0);
            auto payload = row; for (const auto* field : {"schema_version","record_no","time_ticks","phase","tie_key","action_sequence"}) { payload.erase(field); }
            auto event = decode(payload,at); require(encode(event) == payload);
            sink.on_event({at,phase,tie,sequence},event);
        } catch (const std::exception& error) { throw std::runtime_error("events.jsonl: line "+std::to_string(number)+": "+error.what()); }
    }
    if (input.bad() || !input.eof() || !completed) { throw std::runtime_error("events.jsonl: read error or missing completion marker"); }
    return *completed;
}
}  // namespace telemetry::io
