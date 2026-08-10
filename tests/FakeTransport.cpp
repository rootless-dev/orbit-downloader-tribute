#include "FakeTransport.h"

Probe* FakeTransport::createProbe(const EngineConfig& cfg, QObject* parent) {
    Q_UNUSED(cfg);
    return new FakeProbe(m_probeResult, &m_lastCreds, &m_probeCount, parent);
}

SegmentSource* FakeTransport::createWorker(QFile* file, const EngineConfig& cfg,
                                           RateLimiter* limiter, QObject* parent) {
    Q_UNUSED(cfg);
    Q_UNUSED(limiter);   // FakeTransport doesn't exercise rate limiting
    if (m_restartOnce) return new RestartingWorker(m_body, file, parent);
    if (m_steppedBase > 0) {
        const qint64 step = qMax<qint64>(1, m_steppedBase / (m_created + 1));
        ++m_created;
        return new SteppedWorker(m_body, file, step, &m_live, &m_peakLive, parent);
    }
    ++m_created;
    return new FakeWorker(m_body, file, parent);
}
