#include "console_report.hpp"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>

namespace telemetry::console {
namespace {
constexpr std::array names{"fixed_rr","fixed_ll","adaptive_rr","adaptive_ll"};
std::string number(long double value) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(8) << value; return out.str();
}
std::string metric(std::optional<long double> value) { return value ? number(*value) : "нет данных"; }
}
std::string_view variant_name(simulation::Variant variant) { return names.at(static_cast<std::size_t>(variant)); }
void configuration(std::ostream& out,const simulation::Scenario& scenario) {
    const auto& physical = *scenario.physical(); const auto& timeout = scenario.timeout(); const auto& adaptive = scenario.adaptive();
    out << "\nКонфигурация эксперимента (1 тик = 1 мкс):\n"
        << "  Узлов: " << physical.nodes().size() << "; задач: " << scenario.tasks().size()
        << "; горизонт: " << physical.horizon().ticks() << " мкс\n";
    for (const auto& node : physical.nodes()) {
        out << "  Узел " << node.id.value() << ": производительность " << number(node.performance) << " ед. работы/с\n";
    }
    if (!scenario.tasks().empty()) {
        const auto [low,high] = std::minmax_element(scenario.tasks().begin(),scenario.tasks().end(),[](const auto& a,const auto& b) { return a.work < b.work; });
        out << "  Работа задачи: " << number(low->work) << ".." << number(high->work)
            << "; поступления: " << scenario.tasks().front().arrival.ticks() << ".." << scenario.tasks().back().arrival.ticks() << " мкс\n";
    }
    out << "  H (период heartbeat): " << timeout.heartbeat_period.ticks()
        << "; tau0 (начальный порог SUSPECT): " << timeout.initial_timeout.ticks()
        << " мкс; kappa (множитель порога FAILED): " << number(timeout.kappa) << '\n'
        << "  Adaptive: M (размер окна)=" << adaptive.window_size << ", min_intervals (прогрев)=" << adaptive.min_intervals
        << ", c (запас на разброс)=" << number(adaptive.coefficient) << '\n';
    const auto lost = std::count_if(physical.deliveries().begin(),physical.deliveries().end(),[](const auto& row) { return row.lost; });
    out << "  Трасса: " << physical.deliveries().size() << " потенциальных сообщений, " << lost << " помечено потерянными (до crash/blackout).\n";
    if (!physical.deliveries().empty()) {
        const auto [low,high] = std::minmax_element(physical.deliveries().begin(),physical.deliveries().end(),[](const auto& a,const auto& b) { return a.delay < b.delay; });
        out << "  Задержка по трассе: " << low->delay.ticks() << ".." << high->delay.ticks() << " мкс\n";
    }
    out << "  Настоящих отказов: " << physical.crashes().size() << "; интервалов blackout: " << physical.blackouts().size() << '\n';
    for (const auto& crash : physical.crashes()) { out << "    Crash узла " << crash.node.value() << " в " << crash.at.ticks() << " мкс\n"; }
    for (const auto& blackout : physical.blackouts()) { out << "    Blackout узла " << blackout.node.value() << ": [" << blackout.start.ticks() << ", " << blackout.end.ticks() << ") мкс\n"; }
    for (const auto& window : scenario.measurement_windows()) { out << "  Окно throughput: (" << window.start.ticks() << ", " << window.end.ticks() << "] мкс\n"; }
}
void result(std::ostream& out,std::string_view name,const metrics::Summary& s) {
    out << "\nРезультаты: " << name << '\n'
        << "  Поступило: " << s.arrived << "; завершено: " << s.completed << "; не завершено: " << s.unfinished
        << " (в общей очереди: " << s.queued << ", назначено: " << s.assigned << ")\n"
        << "  Повторных назначений: " << s.retries << "; отклонённых результатов отозванных попыток: " << s.rejected_revoked_results << '\n'
        << "  Latency (от поступления до результата), мкс: среднее=" << metric(s.mean_latency_ticks)
        << ", P95=" << (s.p95_latency ? std::to_string(s.p95_latency->ticks()) : "нет данных") << "; выборка=" << s.completed << '\n'
        << "  Detection (от crash до FAILED), мкс: " << metric(s.mean_detection_ticks) << "; наблюдений=" << s.detection_samples << '\n'
        << "  Recovery (от crash до завершения возвращённой работы), мкс: " << metric(s.mean_recovery_ticks) << "; наблюдений=" << s.recovery_samples << '\n'
        << "  FP (ложные исключения): " << s.false_positives << "; FN (необнаруженные отказы): " << s.false_negatives << '\n'
        << "  Crash после ложного исключения: " << s.crashes_after_false_exclusion << "; незавершённых Recovery: " << s.incomplete_recoveries << '\n';
    for (const auto& t : s.throughput) { out << "  Throughput (" << t.window.start.ticks() << ", " << t.window.end.ticks() << "]: " << number(t.tasks_per_second) << " задач/с; завершений=" << t.completed << '\n'; }
    if (s.throughput.empty()) { out << "  Throughput: окна измерения не заданы.\n"; }
    if (s.arrived == 0) { out << "  Наблюдение: задач не поступало; задержки не определены.\n"; }
    else if (s.unfinished) { out << "  Наблюдение: на горизонте остались задачи; задержки рассчитаны только по завершённым.\n"; }
    else { out << "  Наблюдение: все поступившие задачи завершены.\n"; }
    if (s.crashes == 0) { out << "  Настоящих отказов не было; Detection и Recovery не применимы.\n"; }
    else if (!s.detection_samples || !s.recovery_samples) { out << "  Отсутствующие Detection/Recovery не равны нулю; причины по узлам — в nodes.csv.\n"; }
}
void comparison(std::ostream& out,const io::Comparison& summaries) {
    for (std::size_t i = 0; i < summaries.size(); ++i) { result(out,names[i],summaries[i]); }
    const auto complete = std::all_of(summaries.begin(),summaries.end(),[&](const auto& s) {
        return s.arrived > 0 && s.arrived == summaries.front().arrived && s.unfinished == 0 && s.p95_latency.has_value();
    });
    out << "\nНаблюдения по сравнению:\n";
    if (complete) {
        const auto best = std::min_element(summaries.begin(),summaries.end(),[](const auto& a,const auto& b) { return a.p95_latency < b.p95_latency; });
        out << "  Минимальный P95 на этом входе: " << best->p95_latency->ticks() << " мкс; варианты:";
        for (std::size_t i = 0; i < summaries.size(); ++i) { if (summaries[i].p95_latency == best->p95_latency) { out << ' ' << names[i]; } }
        out << ".\n";
    } else { out << "  Общий минимум задержки не выделяется: есть незавершённые задачи или пустые выборки.\n"; }
    out << "  Сопоставляйте задержки вместе с завершённостью, FP/FN и повторами.\n"
        << "  Это результат данного опыта, не доказательство общего превосходства алгоритма.\n";
}
}  // namespace telemetry::console
