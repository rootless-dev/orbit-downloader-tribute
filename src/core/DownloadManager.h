#pragma once
#include "DownloadTypes.h"
#include "AbstractTask.h"
#include "DownloadTask.h"
#include "Transport.h"
#include "RateLimiter.h"
#include <QObject>
#include <QVector>
#include <QHash>
#include <QSet>
#include <memory>
#include <vector>

class Logger;
class QNetworkAccessManager;
class TorrentTask;
struct TorrentMetainfo;

class DownloadManager : public QObject {
    Q_OBJECT
public:
    DownloadManager(const EngineConfig& cfg, const QString& dataDir,
                    Logger* logger = nullptr, QObject* parent = nullptr);
    QUuid addDownload(const QUrl& url, const QString& destPath, const HeaderList& extraHeaders = {},
                      bool provisionalName = false);
    // Parses `torrentPath`, copies it into <dataDir>/torrents/<hexInfoHash>.torrent
    // (so resume never depends on the user's original file), and constructs +
    // wires a TorrentTask. Returns a null QUuid (nothing added) if the file
    // can't be read or fails to parse as a valid .torrent. A second call with
    // the same info-hash returns the existing task's id instead of duplicating it.
    QUuid addTorrent(const QString& torrentPath, const QString& destDir,
                      const QSet<int>& selectedFiles, PieceStrategy strategy);
    void  pauseAll();
    void  resumeAll();
    void  remove(const QUuid& id, bool deleteFiles);
    void  loadSession();
    void  setConfig(const EngineConfig& cfg);   // banda + cap ao vivo; resto p/ próximos downloads
    // Plain values (not the GUI's BitTorrentPrefs struct — core can't depend on
    // gui/Settings.h): applies to torrents added/resumed AFTER this call, same
    // "resto p/ próximos downloads" contract as setConfig() above.
    void  setTorrentDefaults(int maxPeersPerTorrent, quint16 listenPort, ResumeVerifyMode verify);
    QVector<AbstractTask*> tasks() const { return m_tasks; }
    AbstractTask* taskById(const QUuid& id) const;
    void  pause(const QUuid& id);
    void  resume(const QUuid& id);
    void  cancel(const QUuid& id);
    void  setPriority(const QUuid& id, Priority p);
    bool  moveFiles(const QUuid& id, const QString& newDir);
    bool  retarget(const QUuid& id, const QString& newDestPath);
    void  provideCredentials(const QUuid& id, const QString& user, const QString& pass);
    Transport* transportFor(const QUrl& url) const;   // nullptr se esquema desconhecido
signals:
    void taskProgress(const QUuid& id, qint64 received, qint64 total);
    void taskStateChanged(const QUuid& id, DownloadState state);
    void credentialsRequired(const QUuid& id, const QString& host);
private:
    QString sessionPath() const;
    QString torrentsSessionPath() const;   // separate file: <dataDir>/torrents.json
    QString torrentsDir() const;           // <dataDir>/torrents (copied .torrent + resume files)
    void    saveSession();
    void    saveTorrentSession();
    void    loadTorrentSession();
    void    pump();                 // promote Queued -> Downloading up to maxConcurrent
    void    wire(AbstractTask* t);
    // Shared TorrentTask construction (addTorrent() + loadTorrentSession()):
    // one place for the listen-port/maxPeers/verify-mode defaults and the
    // rngSeed/nam/limiter/logger/resumeDir wiring, so the two call sites can't
    // drift once EngineConfig eventually grows torrent-specific fields.
    TorrentTask* makeTorrentTask(const TorrentMetainfo& meta, const QString& destDir,
                                 const QSet<int>& selectedFiles, PieceStrategy strategy);

    // Per-torrent fields addTorrent()/loadTorrentSession() know but TorrentTask
    // itself doesn't re-expose (destDir/selectedFiles/strategy as originally
    // requested) - kept here, keyed by task id, purely for session round-trips.
    struct TorrentSessionMeta {
        QString    destDir;
        QSet<int>  selectedFiles;
        PieceStrategy strategy = PieceStrategy::RarestFirst;
    };
    QHash<QUuid, TorrentSessionMeta> m_torrentMeta;

    EngineConfig            m_cfg;
    QString                 m_dataDir;
    // Torrent defaults threaded through makeTorrentTask(); initialized to the
    // engine's previous hardcoded values, overridable via setTorrentDefaults().
    int                     m_torrentMaxPeers    = 50;
    quint16                 m_torrentListenPort  = 6881;
    ResumeVerifyMode        m_torrentVerify      = ResumeVerifyMode::TrustBitfield;
    RateLimiter             m_limiter;       // teto global de banda, consultado pelos workers
    QHash<QString, Transport*>              m_transports;   // scheme -> transport (não-dono)
    std::vector<std::unique_ptr<Transport>> m_owned;        // dono de verdade
    QVector<AbstractTask*>  m_tasks;
    bool                    m_inPump = false;
    Logger*                 m_logger = nullptr;   // não-dono; pode ser nullptr
    QNetworkAccessManager*  m_torrentNam = nullptr; // owned (parented to this); shared by all TorrentTasks
};
