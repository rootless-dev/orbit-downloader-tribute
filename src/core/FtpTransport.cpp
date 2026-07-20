#include "FtpTransport.h"
#include "FtpProbe.h"
#include "FtpSegmentWorker.h"

Probe* FtpTransport::createProbe(const EngineConfig& cfg, QObject* parent) {
    Q_UNUSED(cfg);   // FTP não tem cabeçalho User-Agent
    return new FtpProbe(m_probeCfg, parent);
}

SegmentSource* FtpTransport::createWorker(QFile* file, const EngineConfig& cfg,
                                          RateLimiter* limiter, QObject* parent) {
    return new FtpSegmentWorker(file, cfg, limiter, parent);
}
