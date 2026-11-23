#include "timeline_widget.hpp"
#include "replay.hpp"

#include <QHelpEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QScrollArea>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>

namespace telemetry::gui {
namespace {
constexpr int axisHeight = 34;
constexpr int trackHeight = 77;
constexpr int laneHeight = 14;

struct TimeScale {
    qreal left, right;
    qint64 horizon;

    qreal operator()(qint64 time) const {
        return left + (right - left) * static_cast<qreal>(static_cast<long double>(time) / horizon);
    }
};

struct MarkStyle {
    QString glyph;
    QColor color;
    int offset;
};

MarkStyle markerStyle(TimelineEvent kind) {
    switch (kind) {
    case TimelineEvent::Crash: return {"▼", QColor("#9f1239"), 12};
    case TimelineEvent::Finished: return {"◆", QColor("#172554"), 32};
    case TimelineEvent::Revoked: return {"×", QColor("#9a3412"), 43};
    case TimelineEvent::Returned: return {"↩︎", QColor("#9a3412"), 56};
    case TimelineEvent::Reassigned: return {"↻", QColor("#1d4ed8"), 56};
    case TimelineEvent::Accepted: return {"●", QColor("#166534"), 69};
    case TimelineEvent::Rejected: return {"○", QColor("#9f1239"), 69};
    case TimelineEvent::DetectorChanged:
    case TimelineEvent::Started: return {};
    }
    return {};
}

QColor stateColor(const QString& state) {
    if (state == "ALIVE") { return QColor("#bbf7d0"); }
    if (state == "SUSPECT") { return QColor("#fde68a"); }
    return QColor("#fecaca");
}

QString eventText(const TimelineMarker& marker) {
    QString action;
    switch (marker.kind) {
    case TimelineEvent::DetectorChanged: action = "Оценка детектора: " + marker.detail; break;
    case TimelineEvent::Crash: action = "Физический отказ"; break;
    case TimelineEvent::Started: action = "Исполнение начато"; break;
    case TimelineEvent::Finished: action = "Физическое исполнение завершено"; break;
    case TimelineEvent::Revoked: action = "Попытка отозвана"; break;
    case TimelineEvent::Returned: action = "Попытка отозвана, задача возвращена в общую очередь"; break;
    case TimelineEvent::Reassigned: action = "Повторное назначение"; break;
    case TimelineEvent::Accepted: action = "Результат принят"; break;
    case TimelineEvent::Rejected: action = "Результат отклонён (" + marker.detail + ")"; break;
    }
    const auto task = marker.task.isEmpty() ? QString{} :
        QString(" · задача %1, попытка %2").arg(marker.task, marker.attempt);
    return QString("№ %1 · Узел %2 · %3%4").arg(marker.at.record).arg(marker.node, action, task);
}

QString legendEntry(TimelineEvent kind, const QString& label) {
    const auto mark = markerStyle(kind);
    return QString("<b style='color:%1'>%2</b> %3").arg(mark.color.name(), mark.glyph, label);
}

void drawBand(QPainter& painter, const QRectF& band, const QColor& color, const QString& text) {
    painter.fillRect(band, color);
    const auto available = static_cast<int>(band.width()) - 6;
    if (available < painter.fontMetrics().horizontalAdvance(text)) { return; }
    painter.setPen(QColor("#172033"));
    painter.drawText(band, Qt::AlignCenter, text);
}
}

class TimelineCanvas : public QWidget {
public:
    explicit TimelineCanvas(QWidget* parent) : QWidget(parent) {
        setObjectName("timelineCanvas");
        setAccessibleName("Шкала состояний и выполнения задач по узлам");
        setMinimumWidth(290);
    }

    void setData(QSharedPointer<const ReplayData> replay) {
        m_data = std::move(replay);
        m_rows.clear();
        if (m_data) {
            for (qsizetype i = 0; i < m_data->nodes.size(); ++i) {
                m_rows.insert(m_data->nodes[i], axisHeight + static_cast<int>(i) * trackHeight);
            }
        }
        m_time = 0;
        m_record = 0;
        setMinimumHeight(std::max(80, axisHeight + static_cast<int>(m_rows.size()) * trackHeight + 5));
        update();
    }

    void setPosition(qint64 time, qint64 record) {
        m_time = time;
        m_record = record;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), palette().base());
        if (!m_data) {
            painter.setPen(palette().text().color());
            painter.drawText(rect(), Qt::AlignCenter, "Выполните расчёт, чтобы открыть шкалу");
            return;
        }

        const auto xAt = timeToX();
        const auto bottom = axisHeight + static_cast<int>(m_rows.size()) * trackHeight;
        const auto markWidth = painter.fontMetrics().horizontalAdvance(QString::number(m_data->horizon));
        const auto divisions = std::clamp(static_cast<int>((xAt(m_data->horizon) - xAt(0)) / (markWidth + 20)), 1, 5);
        const auto tickCount = std::min<qint64>(divisions, m_data->horizon);
        for (qint64 i = 0; i <= tickCount; ++i) {
            const auto tick = tickCount == 0 ? 0 :
                m_data->horizon / tickCount * i + m_data->horizon % tickCount * i / tickCount;
            const auto x = xAt(tick);
            painter.setPen(palette().midlight().color());
            painter.drawLine(QPointF(x, axisHeight - 6), QPointF(x, bottom));
            painter.setPen(palette().text().color());
            painter.drawText(QRectF(x - markWidth / 2.0 - 2, 3, markWidth + 4, axisHeight - 10),
                Qt::AlignCenter, QString::number(tick));
        }
        painter.drawText(QRectF(4, 3, xAt(0) - 8, axisHeight - 10), Qt::AlignVCenter, "Время, мкс");

        for (const auto& id : m_data->nodes) {
            const auto y = m_rows.value(id);
            const auto& node = m_data->timeline.nodes[id];
            painter.setPen(palette().text().color());
            const auto label = painter.fontMetrics().elidedText("Узел " + id, Qt::ElideRight, static_cast<int>(xAt(0)) - 12);
            painter.drawText(QRectF(4, y + 3, xAt(0) - 8, laneHeight), Qt::AlignVCenter, label);
            painter.drawText(QRectF(4, y + 24, xAt(0) - 8, laneHeight), Qt::AlignVCenter, "Попытки");
            painter.setPen(palette().midlight().color());
            painter.drawLine(4, y + trackHeight - 1, width() - 4, y + trackHeight - 1);

            for (qsizetype i = 0; i < node.states.size(); ++i) {
                const auto& transition = node.states[i];
                if (transition.at.record > m_record) { break; }
                const auto end = i + 1 < node.states.size() && node.states[i + 1].at.record <= m_record
                    ? node.states[i + 1].at.time : m_time;
                const auto start = xAt(transition.at.time);
                drawBand(painter, QRectF(start, y + 4, std::max(1., xAt(end) - start), laneHeight),
                    stateColor(transition.state), transition.state);
            }
            for (const auto& attempt : node.attempts) {
                if (attempt.start.record > m_record) { break; }
                const auto end = attempt.end && attempt.end->record <= m_record ? attempt.end->time : m_time;
                const auto start = xAt(attempt.start.time);
                drawBand(painter, QRectF(start, y + 24, std::max(1., xAt(end) - start), laneHeight),
                    QColor("#bfdbfe"), QString("%1 / %2").arg(attempt.task, attempt.number));
            }
        }

        painter.setPen(QPen(palette().highlight().color(), 1, Qt::DashLine));
        const auto cursor = xAt(m_time);
        painter.drawLine(QPointF(cursor, axisHeight - 6), QPointF(cursor, bottom));
        for (const auto& marker : m_data->timeline.markers) {
            if (marker.at.record > m_record) { break; }
            const auto mark = markerStyle(marker.kind);
            if (mark.glyph.isEmpty()) { continue; }
            painter.setPen(mark.color);
            painter.drawText(QRectF(xAt(marker.at.time) - 7, m_rows.value(marker.node) + mark.offset - 8, 14, 16),
                Qt::AlignCenter, mark.glyph);
        }
    }

    bool event(QEvent* event) override {
        if (event->type() != QEvent::ToolTip || !m_data) { return QWidget::event(event); }
        const auto* help = static_cast<QHelpEvent*>(event);
        const auto& markers = m_data->timeline.markers;
        const auto xAt = timeToX();
        const auto first = std::lower_bound(markers.cbegin(), markers.cend(), help->pos().x() - 6,
            [xAt](const TimelineMarker& marker, int x) { return xAt(marker.at.time) < x; });
        QStringList lines;
        for (auto it = first; it != markers.cend() && it->at.record <= m_record && xAt(it->at.time) <= help->pos().x() + 6; ++it) {
            const auto y = m_rows.value(it->node);
            if (help->pos().y() >= y && help->pos().y() < y + trackHeight) {
                lines.append(QString("%1 мкс · %2").arg(it->at.time).arg(eventText(*it)));
                if (lines.size() == 12) { break; }
            }
        }
        if (lines.isEmpty()) {
            QToolTip::hideText();
            event->ignore();
        } else {
            QToolTip::showText(help->globalPos(), lines.join('\n'), this);
        }
        return true;
    }

private:
    TimeScale timeToX() const {
        constexpr qreal left = 86;
        const auto right = std::max<qreal>(left + 1, width() - std::max(14,
            6 + fontMetrics().horizontalAdvance(QString::number(m_data->horizon)) / 2));
        return {left, right, std::max<qint64>(1, m_data->horizon)};
    }

    QSharedPointer<const ReplayData> m_data;
    QMap<QString,int> m_rows;
    qint64 m_time = 0;
    qint64 m_record = 0;
};

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
    setObjectName("timeline");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto* legend = new QLabel(this);
    legend->setTextFormat(Qt::RichText);
    legend->setWordWrap(true);
    legend->setText(QString(
        "<span style='background-color:#bbf7d0;color:#172033'>&nbsp;ALIVE&nbsp;</span>&nbsp;"
        "<span style='background-color:#fde68a;color:#172033'>&nbsp;SUSPECT&nbsp;</span>&nbsp;"
        "<span style='background-color:#fecaca;color:#172033'>&nbsp;FAILED&nbsp;</span>&nbsp;"
        "<span style='background-color:#bfdbfe;color:#172033'>&nbsp;Задача / попытка&nbsp;</span>&nbsp;"
        "┆ Позиция просмотра<br>%1 · %2 · %3 · %4 · %5 · %6 · %7")
        .arg(legendEntry(TimelineEvent::Crash, "отказ"), legendEntry(TimelineEvent::Revoked, "отзыв"),
             legendEntry(TimelineEvent::Returned, "отзыв и возврат"), legendEntry(TimelineEvent::Reassigned, "переназначение"),
             legendEntry(TimelineEvent::Finished, "физическое завершение"),
             legendEntry(TimelineEvent::Accepted, "результат принят"), legendEntry(TimelineEvent::Rejected, "отклонён")));
    layout->addWidget(legend);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setMinimumHeight(158);
    m_canvas = new TimelineCanvas(scroll);
    scroll->setWidget(m_canvas);
    layout->addWidget(scroll, 1);

    m_caption = new QLabel("События на текущей отметке", this);
    layout->addWidget(m_caption);
    m_events = new QListWidget(this);
    m_events->setObjectName("timelineEvents");
    m_events->setAccessibleName("События на текущей отметке в порядке журнала");
    m_events->setUniformItemSizes(true);
    m_events->setFixedHeight(118);
    layout->addWidget(m_events);
}

void TimelineWidget::setData(QSharedPointer<const ReplayData> replay) {
    m_data = std::move(replay);
    m_time = -1;
    m_record = -1;
    m_canvas->setData(m_data);
    setPosition(0, 0);
}

void TimelineWidget::setPosition(qint64 time, qint64 record) {
    if (time == m_time && record == m_record) { return; }
    const bool timeChanged = time != m_time;
    m_time = time;
    m_record = record;
    m_canvas->setPosition(time, record);
    m_caption->setText(m_data ? QString("События на отметке %1 мкс").arg(time) : "События на текущей отметке");
    m_events->clear();
    if (!m_data) { return; }

    const auto& markers = m_data->timeline.markers;
    const auto first = std::lower_bound(markers.cbegin(), markers.cend(), time,
        [](const TimelineMarker& marker, qint64 value) { return marker.at.time < value; });
    for (auto it = first; it != markers.cend() && it->at.time == time && it->at.record <= record; ++it) {
        m_events->addItem(eventText(*it));
    }
    if (!timeChanged) { m_events->scrollToBottom(); }
}
} // namespace telemetry::gui
