#include "RateLimiter.h"

void RateLimiter::setRate(qint64 bytesPerSec) {
    m_ratePerSec = bytesPerSec > 0 ? bytesPerSec : 0;
    m_burst      = double(m_ratePerSec);           // burst = 1s
    if (m_tokens > m_burst) m_tokens = m_burst;
}

qint64 RateLimiter::take(qint64 want, qint64 nowMs) {
    if (want <= 0) return 0;
    if (m_ratePerSec <= 0) return want;            // ilimitado
    if (!m_primed) { m_primed = true; m_lastMs = nowMs; m_tokens = m_burst; }  // prime cheio
    const qint64 elapsed = nowMs - m_lastMs;
    if (elapsed > 0) {
        m_tokens = qMin(m_burst, m_tokens + double(m_ratePerSec) * double(elapsed) / 1000.0);
        m_lastMs = nowMs;
    }
    qint64 grant = qint64(qMin(m_tokens, double(want)));
    if (grant < 0) grant = 0;
    m_tokens -= double(grant);
    return grant;
}
