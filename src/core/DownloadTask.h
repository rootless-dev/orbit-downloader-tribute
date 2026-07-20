#pragma once
#include "DownloadTypes.h"
#include "Persistence.h"
#include "Transport.h"
#include <QObject>
#include <QTimer>
#include <QVector>
class QFile;
class RateLimiter;
class Logger;
enum class LogLevel;

class DownloadTask : public QObject {
    Q_OBJECT
public:
    DownloadTask(Transport* transport, const EngineConfig& cfg,
                 RateLimiter* limiter = nullptr, QObject* parent = nullptr);
    void init(const QUuid& id, const QUrl& url, const QString& destPath, int segmentCount,
              const HeaderList& extraHeaders = {}, bool provisionalName = false);
    void restore(const DownloadRecord& rec, const QVector<Segment>& segs,
                 const QString& etag, const QString& lastModified, bool validated);
    void start();
    void pause();
    void requeue();
    void cancel();
    void setDestPath(const QString& path);
    void setCredentials(const Credentials& c);
    DownloadState    state() const { return m_state; }
    QString          error() const { return m_error; }
    QUuid            id() const { return m_id; }
    DownloadRecord   record() const;
    QVector<Segment> segments() const;
    Priority priority() const { return m_priority; }
    void     setPriority(Priority p) { m_priority = p; }
    void     setLogger(Logger* l) { m_logger = l; }
signals:
    void progress(qint64 received, qint64 total);
    void stateChanged(DownloadState state);
    void segmentProgress(int index, qint64 currentOffset);
    void credentialsRequired(const QUuid& id, const QString& host);
private:
    void setState(DownloadState s);
    void askForCredentials();
    void onProbed(const ProbeResult& r);
    void beginSegments();
    void spawnWorker(const Segment& seg);
    void onSegmentCompleted(int index);
    void onSegmentFailed(int index, const QString& error, FailureKind kind);
    void onRestartRequired(int index);
    void checkAllComplete();
    qint64 receivedBytes() const;
    void emitProgressNow();
    void logLine(LogLevel level, const QString& msg);

    Transport*             m_transport;
    EngineConfig           m_cfg;
    RateLimiter*           m_limiter;
    QUuid                  m_id;
    QUrl                   m_url;
    QString                m_destPath;
    HeaderList             m_extraHeaders;
    bool                   m_provisionalName = false;   // adota nome do Content-Disposition no probe
    int                    m_segmentCount = 4;
    qint64                 m_totalBytes = -1;
    bool                   m_supportsRange = false;
    QString                m_etag, m_lastModified;
    bool                   m_validated = false;
    bool                   m_probed = false;
    QString                m_error;
    DownloadState          m_state = DownloadState::Queued;
    QVector<Segment>       m_segments;
    QVector<SegmentSource*> m_workers;
    QFile*                 m_file = nullptr;
    int                    m_completedCount = 0;
    QTimer*                m_metaTimer = nullptr;
    QTimer*                m_progressTimer = nullptr;
    bool                   m_progressPending = false;
    bool                   m_restarting = false;
    Credentials            m_creds;
    bool                   m_awaitingCredentials = false;
    Priority               m_priority = Priority::Normal;
    Logger*                m_logger = nullptr;
};
