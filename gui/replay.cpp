#include "replay.hpp"
#include <algorithm>
#include <cmath>

namespace telemetry::gui {
void projectEvent(ReplayState& state, const ReplayEvent& event) {
    state.time = event.time;state.record = event.record;
    const auto history = [&](const QString& task, const QString& number, const QString& node, const QString& description) {
        if (!task.isEmpty()) { state.tasks[task].history.append({event.record,event.time,number,node,description}); }
    };
    if (event.kind == "crash") {
        state.nodes[event.node].crashed = true;
        history(event.task,event.attempt,event.node,"Физическое исполнение прервано отказом");
    } else if (event.kind == "heartbeat_observed" || event.kind == "timeout_observed") {
        state.nodes[event.node].detector = event.after;
    } else if (event.kind == "task_arrived") {
        state.queued.insert(event.task);state.tasks[event.task].status = "В общей очереди";
        history(event.task,{},{},"Задача поступила в очередь");
    } else if (event.kind == "task_assigned") {
        state.queued.remove(event.task);auto& task = state.tasks[event.task];task.status = "Назначена";task.attempt = event.attempt;task.node = event.node;
        history(event.task,event.attempt,event.node,event.attempt == "1" ? "Назначена попытка" : "Повторное назначение");
    } else if (event.kind == "reconfiguration" || event.kind == "assignment_revoked") {
        auto revoked = event.revoked;
        if (event.kind == "assignment_revoked") { revoked.append({event.task,event.node,event.attempt}); }
        for (const auto& ref : revoked) {
            state.queued.insert(ref.task);state.tasks[ref.task].status = "В общей очереди";
            history(ref.task,ref.attempt,ref.node,"Попытка отозвана; задача возвращена в очередь");
        }
    } else if (event.kind == "result") {
        if (event.decision == "accepted") { state.tasks[event.task].status = "Завершена"; }
        const QMap<QString,QString> reasons{{"unknown_task","неизвестная задача"},{"already_completed","задача уже завершена"},
            {"no_current_assignment","нет актуального назначения"},{"attempt_mismatch","другая актуальная попытка"},{"node_mismatch","другой исполнитель"}};
        history(event.task,event.attempt,event.node,event.decision == "accepted" ? "Результат принят (accepted)"
            : "Результат отклонён: " + reasons.value(event.decision,event.decision) + " (" + event.decision + ")");
    } else if (event.kind == "physical_started" || event.kind == "physical_finished" || event.kind == "completion_skipped") {
        const auto description = event.kind == "physical_started" ? "Исполнение начато"
            : event.kind == "physical_finished" ? "Физическое исполнение закончено" : "Завершение пропущено: " + event.decision;
        history(event.task,event.attempt,event.node,description);
    }
}
ReplayController::ReplayController(QObject* parent) : QObject(parent) {
    m_timer.setSingleShot(true);m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer,&QTimer::timeout,this,&ReplayController::tick);
}
void ReplayController::setData(QSharedPointer<const ReplayData> data) { pause();m_data = std::move(data);reset(); }
void ReplayController::reset() {
    pause();m_state = {};m_next = 0;m_position = 0;
    if (m_data) {
        for (const auto& node : m_data->nodes) { m_state.nodes.insert(node,{}); }
        for (const auto& task : m_data->tasks) { m_state.tasks.insert(task,{}); }
    }
    emit changed();
}
double ReplayController::gapMilliseconds(qint64 delta, qint64 horizon, double speed) {
    if (delta < 0 || horizon <= 0 || !std::isfinite(speed) || speed <= 0) { return 0; }
    return static_cast<double>(static_cast<long double>(delta)/static_cast<long double>(horizon)*8000.L/static_cast<long double>(speed));
}
void ReplayController::sync() {
    if (!m_playing || !m_data) { return; }
    const auto elapsed = m_clock.nsecsElapsed();m_clock.restart();
    m_position = std::min(static_cast<long double>(m_data->horizon),m_position + static_cast<long double>(elapsed)
        /8000000000.L*static_cast<long double>(m_data->horizon)*static_cast<long double>(m_speed));
}
void ReplayController::play() {
    if (m_playing || !m_data || finished()) { return; }
    m_playing = true;m_clock.start();emit playingChanged(true);schedule();
}
void ReplayController::pause() {
    if (!m_playing) { return; }
    sync();m_timer.stop();m_playing = false;emit playingChanged(false);
}
void ReplayController::setSpeed(double speed) {
    if (!std::isfinite(speed) || speed <= 0) { return; }
    sync();m_speed = speed;if (m_playing) { schedule(); }
}
void ReplayController::apply() {
    projectEvent(m_state,m_data->events[m_next++]);emit recordApplied(m_state.record);
}
bool ReplayController::step() {
    pause();if (!m_data || finished()) { return false; }
    apply();m_position = static_cast<long double>(m_state.time);emit changed();return true;
}
void ReplayController::schedule() {
    m_timer.stop();sync();
    if (finished()) { pause();emit changed();return; }
    const auto distance = std::max(0.L,static_cast<long double>(m_data->events[m_next].time)-m_position);
    const auto delay = distance/static_cast<long double>(m_data->horizon)*8000.L/static_cast<long double>(m_speed);
    m_timer.start(static_cast<int>(std::clamp(std::ceil(delay),0.L,2147483647.L)));
}
void ReplayController::tick() {
    if (!m_playing || !m_data) { return; }
    sync();int applied = 0;
    while (!finished() && static_cast<long double>(m_data->events[m_next].time) <= m_position && applied < 256) { apply();++applied; }
    emit changed();schedule();
}
} // namespace telemetry::gui
