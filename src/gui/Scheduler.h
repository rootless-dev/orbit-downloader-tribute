#pragma once
#include "Settings.h"   // SchedulerConfig, Recurrence
#include <QDateTime>

enum class SchedAction { None, StartAll, StopAll };

// Puro: decide a ação devida por tick, com tempo injetado. Edge-trigger — dispara
// StartAll ao entrar na janela e StopAll após o fim, uma vez por dia (Daily) ou
// uma única vez (Once).
class Scheduler {
public:
    void setConfig(const SchedulerConfig& c) {
        m_cfg = c;
        m_lastStartDate = QDate();   // re-arma: uma mudança de config reavalia a janela do zero
        m_lastStopDate  = QDate();
    }
    SchedAction tick(const QDateTime& now);
private:
    SchedulerConfig m_cfg;
    QDate           m_lastStartDate;
    QDate           m_lastStopDate;
};
