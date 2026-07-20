#pragma once
#include "Transport.h"
class QNetworkAccessManager;
class QObject;

class HttpTransport : public Transport {
public:
    // namParent é o QObject dono do QNetworkAccessManager (tipicamente o
    // DownloadManager) — o QNAM segue na árvore de ownership do Qt, como hoje.
    explicit HttpTransport(QObject* namParent);
    Probe*         createProbe(const EngineConfig& cfg, QObject* parent) override;
    SegmentSource* createWorker(QFile* file, const EngineConfig& cfg,
                                RateLimiter* limiter, QObject* parent) override;
private:
    QNetworkAccessManager* m_nam;
};
