#pragma once
#include "Settings.h"   // SchedulerConfig, Recurrence
#include <QDialog>
#include <QTime>
class QCheckBox;
class QTimeEdit;
class QRadioButton;

class SchedulerDialog : public QDialog {
    Q_OBJECT
public:
    explicit SchedulerDialog(const SchedulerConfig& current, QWidget* parent = nullptr);
    SchedulerConfig result() const;

    // Hooks de teste (sem exec()):
    void setEnabledForTest(bool on);
    void setStartForTest(QTime t);
    void setStopForTest(QTime t);
    void setRecurrenceForTest(Recurrence r);
    void setQuitWhenDoneForTest(bool on);

private:
    QCheckBox*    m_enabled = nullptr;
    QTimeEdit*    m_start   = nullptr;
    QTimeEdit*    m_stop    = nullptr;
    QRadioButton* m_daily   = nullptr;
    QRadioButton* m_once    = nullptr;
    QCheckBox*    m_quit    = nullptr;
};
