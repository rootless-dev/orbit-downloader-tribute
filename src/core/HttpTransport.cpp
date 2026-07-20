#include "HttpTransport.h"
#include "HttpProbe.h"
#include "SegmentWorker.h"
#include <QNetworkAccessManager>

HttpTransport::HttpTransport(QObject* namParent)
    : m_nam(new QNetworkAccessManager(namParent)) {}

Probe* HttpTransport::createProbe(const EngineConfig& cfg, QObject* parent) {
    return new HttpProbe(m_nam, cfg.userAgent.toUtf8(), parent);
}

SegmentSource* HttpTransport::createWorker(QFile* file, const EngineConfig& cfg,
                                           RateLimiter* limiter, QObject* parent) {
    return new SegmentWorker(m_nam, file, cfg, limiter, parent);
}
