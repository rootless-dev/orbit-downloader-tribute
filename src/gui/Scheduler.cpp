#include "Scheduler.h"

SchedAction Scheduler::tick(const QDateTime& now) {
    if (!m_cfg.enabled) return SchedAction::None;
    const QDate today = now.date();
    const QTime t     = now.time();
    const bool inWindow = (t >= m_cfg.start && t < m_cfg.stop);

    if (inWindow && m_lastStartDate != today) {
        m_lastStartDate = today;
        return SchedAction::StartAll;
    }
    if (!inWindow && t >= m_cfg.stop
        && m_lastStartDate == today && m_lastStopDate != today) {
        m_lastStopDate = today;
        if (m_cfg.recurrence == Recurrence::Once) m_cfg.enabled = false;
        return SchedAction::StopAll;
    }
    return SchedAction::None;
}
