#pragma once
#include "Transport.h"

class FtpTransport : public Transport {
public:
    FtpTransport() = default;
    Probe*         createProbe(const EngineConfig& cfg, QObject* parent) override;
    SegmentSource* createWorker(QFile* file, const EngineConfig& cfg,
                                RateLimiter* limiter, QObject* parent) override;
private:
    EngineConfig m_probeCfg;   // só p/ timeouts do probe; o worker recebe o cfg da task
};
