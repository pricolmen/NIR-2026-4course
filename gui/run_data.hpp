#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include "replay.hpp"

namespace telemetry::gui {
struct RunRequest {
    QString scenario;
    QString program;
    QString outputRoot;
    QString detector = "fixed";
    QString scheduler = "rr";
};
struct InputInfo {
    QString scenario;
    QString delivery;
    qint64 horizon = 0;
    qsizetype nodes = 0;
    qsizetype tasks = 0;
};
struct PreparedRun {
    RunRequest request;
    InputInfo input;
    QByteArray scenarioHash;
    QByteArray deliveryHash;
    QString session;
    QString directory;
};
struct RunResult {
    PreparedRun prepared;
    QJsonObject metrics;
    qint64 records = 0;
    QSharedPointer<const ReplayData> playback;
};
InputInfo inspectInput(const QString& path);
PreparedRun prepareRun(RunRequest request);
void ensureUnchanged(const PreparedRun& prepared);
RunResult loadResult(const PreparedRun& prepared);
QByteArray sha256(const QString& path);
QString detectorName(const QString& value);
QString schedulerName(const QString& value);
QString durationText(const QJsonObject& metric);
} // namespace telemetry::gui
Q_DECLARE_METATYPE(telemetry::gui::RunResult)
