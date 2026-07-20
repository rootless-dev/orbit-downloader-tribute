#pragma once
#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QUrl>
#include <QUuid>
#include <QVector>
#include <QList>
#include <QPair>

using HeaderList = QList<QPair<QByteArray, QByteArray>>;

enum class DownloadState { Queued, Connecting, Downloading, Paused, Completed, Error, Cancelled };
enum class Priority { High, Normal, Low };

inline const char* stateName(DownloadState s) {
    switch (s) {
        case DownloadState::Queued:      return "Queued";
        case DownloadState::Connecting:  return "Connecting";
        case DownloadState::Downloading: return "Downloading";
        case DownloadState::Paused:      return "Paused";
        case DownloadState::Completed:   return "Completed";
        case DownloadState::Error:       return "Error";
        case DownloadState::Cancelled:   return "Cancelled";
    }
    return "Unknown";
}

inline QString priorityToString(Priority p) {
    switch (p) {
        case Priority::High:   return QStringLiteral("High");
        case Priority::Low:    return QStringLiteral("Low");
        case Priority::Normal: return QStringLiteral("Normal");
    }
    return QStringLiteral("Normal");
}

inline Priority priorityFromString(const QString& s) {
    if (s == QLatin1String("High")) return Priority::High;
    if (s == QLatin1String("Low"))  return Priority::Low;
    return Priority::Normal;
}

struct Segment {
    int    index   = 0;
    qint64 start   = 0;
    qint64 current = 0;
    qint64 end     = -1;
    qint64 downloaded() const { return current - start; }
    bool   isComplete() const { return end >= 0 && current > end; }
};

struct EngineConfig {
    int    maxConcurrentDownloads = 3;
    int    segmentCount           = 4;
    qint64 minSegSize             = 1LL << 20;
    int    maxSegmentRetries      = 5;
    int    retryBackoffBaseMs     = 1000;
    int    connectTimeoutMs       = 30000;
    int    idleTimeoutMs          = 30000;
    int    progressThrottleMs     = 200;
    qint64 maxBytesPerSec         = 0;              // 0 = ilimitado (teto GLOBAL)
    QString userAgent             = "curl/8.7.1";  // enviado em probe + segmentos HTTP
};

struct ProbeResult {
    bool    ok            = false;
    qint64  totalBytes    = -1;
    bool    supportsRange = false;
    QString etag;
    QString lastModified;
    QUrl    resolvedUrl;
    QString suggestedFileName;        // do Content-Disposition; vazio se ausente
    QString contentType;             // Content-Type da resposta final (diagnóstico)
    int     httpStatus    = 0;       // status HTTP final (diagnóstico)
    QString error;
    bool    authRequired  = false;   // 530: o Core vai pedir credenciais à GUI (spec §3.6)
};

Q_DECLARE_METATYPE(ProbeResult)
