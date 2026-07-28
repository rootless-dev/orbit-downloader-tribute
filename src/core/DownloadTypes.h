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

// Checking and FetchingMetadata are both appended at the END (not
// alphabetized/grouped with the other states): DownloadRecord::state is
// persisted as a raw int (see Persistence::writeSession/readSession), so
// inserting either anywhere else would shift the numeric value of every
// state after it and corrupt any downloads.json/torrents.json written by a
// previous build. Checking = 7. FetchingMetadata = 8 (Task 15: a magnet
// whose info dict hasn't been recovered yet via MetadataFetch).
enum class DownloadState { Queued, Connecting, Downloading, Paused, Completed, Error, Cancelled,
                            Checking, FetchingMetadata };
enum class Priority { High, Normal, Low };
enum class PieceStrategy { RarestFirst, Sequential };
enum class ResumeVerifyMode { TrustBitfield, RecheckOnOpen };

// Per-piece lifecycle state for a torrent download, surfaced to the GUI grid
// (Task 10). Missing = not yet obtained; InFlight = one or more of its blocks
// have been requested from a peer; Have = downloaded, hash-verified, written.
enum class PieceState { Missing, InFlight, Have };

inline const char* stateName(DownloadState s) {
    switch (s) {
        case DownloadState::Queued:      return "Queued";
        case DownloadState::Connecting:  return "Connecting";
        case DownloadState::Downloading: return "Downloading";
        case DownloadState::Paused:      return "Paused";
        case DownloadState::Completed:   return "Completed";
        case DownloadState::Error:       return "Error";
        case DownloadState::Cancelled:   return "Cancelled";
        case DownloadState::Checking:    return "Checking";
        case DownloadState::FetchingMetadata: return "Resolving magnet…";
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
Q_DECLARE_METATYPE(PieceState)
