#pragma once
#include "scenario_files.hpp"
#include <QDialog>
class QLabel;
class QLineEdit;
class QTableWidget;
namespace telemetry::gui {
class ScenarioFormDialog : public QDialog {
    Q_OBJECT
public:
    explicit ScenarioFormDialog(const FormData& data,QWidget* parent = nullptr);
    FormData data() const;
    void setData(const FormData& data);
    QByteArray config() const { return m_config; }
private:
    QMap<QString,QLineEdit*> m_fields;
    QTableWidget* m_nodes;
    QTableWidget* m_failures;
    QTableWidget* m_blackouts;
    QLabel* m_error;
    QByteArray m_config;
};
class FailureDialog : public QDialog {
    Q_OBJECT
public:
    explicit FailureDialog(const ScenarioDocument& document,QWidget* parent = nullptr);
    QVector<FailureSpec> failures() const { return m_result; }
private:
    ScenarioDocument m_document;
    QTableWidget* m_table;
    QLabel* m_error;
    QVector<FailureSpec> m_result;
};
} // namespace telemetry::gui
