#pragma once
#include "run_data.hpp"
#include <QMap>
#include <QSet>
#include <QTemporaryDir>

namespace telemetry::gui {
struct FailureSpec { quint64 node = 0; qint64 at = 0; bool operator==(const FailureSpec&) const = default; };
using InputRows = QVector<QStringList>;
struct FormData {
    QMap<QString,QString> fields;
    InputRows nodes,failures,blackouts;
};
FormData defaultForm();
QByteArray generatorConfig(const FormData& data);
QVector<FailureSpec> parseFailures(const InputRows& rows,const QSet<quint64>& nodes,qint64 horizon);
struct ScenarioDocument {
    QString source,delivery;
    QByteArray bytes,sourceHash,deliveryHash;
    QSet<quint64> nodes;
    qint64 horizon = 0;
    QVector<FailureSpec> failures;
};
ScenarioDocument readScenarioDocument(const QString& path);
QByteArray editedScenario(const ScenarioDocument& document,const QVector<FailureSpec>& failures,const QString& traceName);
PreparedRun generationWorkspace(const RunRequest& request,const QByteArray& config);
PreparedRun generatedRun(const PreparedRun& generation);
struct CopyWorkspace {
    PreparedRun process;
    ScenarioDocument document;
    QString destination,csvName,json;
    QByteArray jsonHash;
    QSharedPointer<QTemporaryDir> staging;
};
QSharedPointer<CopyWorkspace> prepareCopy(const RunRequest& request,const ScenarioDocument& document,
    const QVector<FailureSpec>& failures,const QString& destination);
QString publishCopy(const QSharedPointer<CopyWorkspace>& copy);
} // namespace telemetry::gui
