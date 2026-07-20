#include "SpeedSampler.h"

void SpeedSampler::addSample(qint64 bytes, qint64 tMs) {
    m_samples.push_back({bytes, tMs});
    while (m_samples.size() > 2 && (tMs - m_samples.first().tMs) > kWindowMs)
        m_samples.removeFirst();
}

double SpeedSampler::bytesPerSec() const {
    if (m_samples.size() < 2) return 0.0;
    const Sample& a = m_samples.first();
    const Sample& b = m_samples.last();
    const qint64 dt = b.tMs - a.tMs;
    if (dt <= 0) return 0.0;
    const qint64 db = b.bytes - a.bytes;
    if (db <= 0) return 0.0;
    return (double(db) * 1000.0) / double(dt);
}

qint64 SpeedSampler::etaSeconds(qint64 totalBytes) const {
    if (totalBytes <= 0) return -1;
    const double bps = bytesPerSec();
    if (bps <= 0.0) return -1;
    const qint64 remaining = totalBytes - m_samples.last().bytes;
    if (remaining <= 0) return 0;
    return qint64(remaining / bps);
}

void SpeedSampler::reset() { m_samples.clear(); }
