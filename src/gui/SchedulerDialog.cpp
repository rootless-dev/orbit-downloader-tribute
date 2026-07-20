#include "SchedulerDialog.h"
#include <QCheckBox>
#include <QTimeEdit>
#include <QRadioButton>
#include <QButtonGroup>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>
#include <QDialogButtonBox>

SchedulerDialog::SchedulerDialog(const SchedulerConfig& c, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Scheduler"));

    m_enabled = new QCheckBox(tr("Enable schedule"), this);
    m_enabled->setChecked(c.enabled);
    m_start = new QTimeEdit(c.start, this); m_start->setDisplayFormat("HH:mm");
    m_stop  = new QTimeEdit(c.stop,  this); m_stop->setDisplayFormat("HH:mm");
    m_daily = new QRadioButton(tr("Daily"), this);
    m_once  = new QRadioButton(tr("Once"),  this);
    auto* recGroup = new QButtonGroup(this);
    recGroup->addButton(m_daily);
    recGroup->addButton(m_once);
    (c.recurrence == Recurrence::Once ? m_once : m_daily)->setChecked(true);
    m_quit  = new QCheckBox(tr("Quit when all downloads finish"), this);
    m_quit->setChecked(c.quitWhenDone);

    auto* form = new QFormLayout;
    form->addRow(m_enabled);
    form->addRow(tr("Start:"), m_start);
    form->addRow(tr("Stop:"),  m_stop);
    auto* recRow = new QWidget(this);
    auto* rl = new QHBoxLayout(recRow); rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(m_daily); rl->addWidget(m_once); rl->addStretch();
    form->addRow(tr("Recurrence:"), recRow);
    form->addRow(m_quit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(buttons);
}

SchedulerConfig SchedulerDialog::result() const {
    SchedulerConfig c;
    c.enabled      = m_enabled->isChecked();
    c.start        = m_start->time();
    c.stop         = m_stop->time();
    c.recurrence   = m_once->isChecked() ? Recurrence::Once : Recurrence::Daily;
    c.quitWhenDone = m_quit->isChecked();
    return c;
}

void SchedulerDialog::setEnabledForTest(bool on)         { m_enabled->setChecked(on); }
void SchedulerDialog::setStartForTest(QTime t)           { m_start->setTime(t); }
void SchedulerDialog::setStopForTest(QTime t)            { m_stop->setTime(t); }
void SchedulerDialog::setRecurrenceForTest(Recurrence r) { (r == Recurrence::Once ? m_once : m_daily)->setChecked(true); }
void SchedulerDialog::setQuitWhenDoneForTest(bool on)    { m_quit->setChecked(on); }
