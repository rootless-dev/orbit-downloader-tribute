#pragma once
#include <QtGlobal>

// Token bucket global. Tempo injetável (nowMs) para teste determinístico, no
// mesmo espírito do SpeedSampler. take() nunca bloqueia: devolve o quanto pode
// conceder agora (0..want). Taxa 0 = ilimitado (bypass, custo zero).
class RateLimiter {
public:
    void   setRate(qint64 bytesPerSec);          // 0 = ilimitado
    qint64 take(qint64 want, qint64 nowMs);
    qint64 rate() const { return m_ratePerSec; }
private:
    qint64 m_ratePerSec = 0;
    double m_tokens      = 0.0;
    double m_burst       = 0.0;   // capacidade = 1s de taxa
    qint64 m_lastMs      = 0;
    bool   m_primed      = false;
};
